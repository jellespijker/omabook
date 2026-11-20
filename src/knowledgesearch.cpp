#include "knowledgesearch.h"

#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <ranges>
#include <tuple>

#include "annotationwords.h"
#include "changelog.h"
#include "graphcontext.h"
#include "graphindex.h"
#include "tenancy.h"

using namespace Qt::StringLiterals;

namespace {

// What is said of a thing is looked in this far, which holds a summary's
// gist and keeps the index small.
constexpr qsizetype saidRoom = 240;

// Whether `word` (case folded) starts a word somewhere in `text` (case
// folded): "venn" in "mara venn", not "enn".
[[nodiscard]] bool startsWordIn(const QString &text, const QString &word) {
    for (qsizetype at = text.indexOf(word); at >= 0; at = text.indexOf(word, at + 1))
        if (at == 0 || !text.at(at - 1).isLetterOrNumber())
            return true;
    return false;
}

// So many things touched that the index is read sooner whole than again in part.
constexpr qsizetype mostRetaken = 2000;
// So many things a search's words may find that it reads them all.
constexpr qsizetype mostCandidates = 20000;

[[nodiscard]] bool same(const GraphContext::Reach &a, const GraphContext::Reach &b) {
    return a.any == b.any && a.all == b.all && a.place.bookId == b.place.bookId && a.place.section == b.place.section
           && a.place.fraction == b.place.fraction;
}

// The things a search for `query` may find by what is known of them: those
// the word index says hold a word that starts one of its words, the key of a
// field left off ("knows:corin" is "corin"). Every word a search asks of the
// index (KnownWords) is made of those, so the things it can find are among
// these. Nothing when a word of it is none to the index, or they are too many.
[[nodiscard]] std::optional<QList<qint64>> candidates(const Library &library, const QString &query) {
    static const QRegularExpression pieces(u"[\\s\"]+"_s);
    static const QRegularExpression fielded(u"^-?[A-Za-z]+:"_s);
    static const QRegularExpression apart(u"[^\\p{L}\\p{N}\\p{M}\\p{Co}]+"_s);
    QStringList phrases;
    for (QString piece : query.split(pieces, Qt::SkipEmptyParts)) {
        if (piece == "OR"_L1)
            continue;
        piece.remove(fielded);
        if (piece.startsWith(u'-'))
            piece.remove(0, 1);
        const QStringList words = piece.split(apart, Qt::SkipEmptyParts);
        if (words.isEmpty() && piece.size() >= 2)
            return std::nullopt;
        for (const QString &word : words)
            phrases << GraphIndex::phrase(word).value_or(QString());
    }
    QList<qint64> things;
    if (phrases.isEmpty())
        return things;
    for (QSqlQuery found =
             ChangeLog::query(library, u"SELECT rowid FROM graph_words WHERE graph_words MATCH ? LIMIT %1"_s.arg(mostCandidates + 1),
                              {u"{name summary data said} : (%1)"_s.arg(phrases.join(u" OR "_s))});
         found.next();)
        things << found.value(0).toLongLong();
    if (things.size() > mostCandidates)
        return std::nullopt;
    std::ranges::sort(things);
    return things;
}

} // namespace

QJsonObject KnownMatch::toJson() const {
    QJsonObject object{{"node", node}, {"kind", kind}, {"name", name}};
    if (met.place.bookId > 0) {
        QJsonObject at{{"book", met.place.bookId}, {"section", met.place.section}, {"fraction", met.place.fraction}};
        if (!met.anchor.isEmpty())
            at.insert("anchor", met.anchor);
        object.insert("met", at);
    }
    if (inSummary)
        object.insert("in_summary", true);
    if (!from.isEmpty())
        object.insert("from", from);
    return object;
}

QVariantMap KnownMatch::toMap() const {
    return {{"node", node},
            {"kind", kind},
            {"name", name},
            {"bookId", bookId},
            {"metIn", met.place.bookId},
            {"section", met.place.section},
            {"fraction", met.place.fraction},
            {"anchor", met.anchor},
            {"inSummary", inSummary},
            {"from", from}};
}

QStringList KnowledgeIndex::kinds() {
    return {"theme", "topic", "concept", "character", "place", "thread", "event", "group", "item", "question"};
}

KnowledgeIndex KnowledgeIndex::build(const Library &library) { return build(library, Tenancy::tenants(library)); }

KnowledgeIndex KnowledgeIndex::build(const Library &library, const QList<Tenant> &tenants) {
    KnowledgeIndex index;
    index.changes = library.changes();
    index.m_root = library.root();
    index.m_tenants = tenants | std::views::transform(&Tenant::globalId) | std::ranges::to<QStringList>();
    index.m_tenantBooks = tenantBooks(library);
    index.m_refills = GraphIndex::tenancyRefills(library);
    const GraphKnown known = index.knownBy(library);
    for (const Tenant &tenant : tenants)
        index.take(Graph::forTenant(library, tenant), known, std::nullopt);
    return index;
}

KnowledgeIndex KnowledgeIndex::since(const Library &library, const KnowledgeIndex *before) {
    if (!before || before->m_root.isEmpty() || before->m_root != library.root())
        return build(library);
    const QList<Tenant> tenants = Tenancy::tenants(library);
    KnowledgeIndex index = *before;
    // Read before what it counts, so that what is written meanwhile is read again next time.
    index.changes = library.changes();
    if ((tenants | std::views::transform(&Tenant::globalId) | std::ranges::to<QStringList>()) != before->m_tenants
        || tenantBooks(library) != before->m_tenantBooks || GraphIndex::tenancyRefills(library) != before->m_refills)
        return build(library, tenants);
    QSet<qint64> things = GraphIndex::touchedSince(library, before->changes.graph);
    // A book read on (or back) in: what has a place in it may be known otherwise now.
    if (index.changes.books != before->changes.books) {
        QList<qint64> readOn;
        for (const auto &[book, was] : before->m_reaches.asKeyValueRange())
            if (const GraphContext::Reach now = GraphContext::reachOf(library, book); !same(now, was)) {
                readOn << book;
                index.m_reaches.insert(book, now);
            }
        if (!readOn.isEmpty())
            for (const Tenant &tenant : tenants)
                things.unite(Graph::forTenant(library, tenant).placedIn(readOn));
    }
    // So much that it is read sooner whole.
    if (things.size() > std::max<qsizetype>(mostRetaken, index.size() / 2))
        return build(library, tenants);
    index.retake(library, tenants, things);
    return index;
}

KnowledgeIndex KnowledgeIndex::forSearch(const Library &library, const QString &query, bool whole) {
    const std::optional<QList<qint64>> things = whole ? std::nullopt : candidates(library, query);
    if (!things)
        return build(library);
    KnowledgeIndex index;
    index.changes = library.changes();
    const GraphKnown known = index.knownBy(library);
    for (const Tenant &tenant : Tenancy::tenants(library))
        index.take(Graph::forTenant(library, tenant), known, things);
    return index;
}

GraphKnown KnowledgeIndex::knownBy(const Library &library) {
    return [this, &library](const GraphPlace &at) {
        if (at.bookId == 0)
            return true;
        auto found = m_reaches.find(at.bookId);
        if (found == m_reaches.end())
            found = m_reaches.insert(at.bookId, GraphContext::reachOf(library, at.bookId));
        return found->covers(at);
    };
}

QList<std::pair<qint64, qint64>> KnowledgeIndex::tenantBooks(const Library &library) {
    QList<std::pair<qint64, qint64>> books;
    for (QSqlQuery query = ChangeLog::query(library, u"SELECT tenant, book_id FROM tenant_books ORDER BY tenant, book_id"_s); query.next();)
        books.append({query.value(0).toLongLong(), query.value(1).toLongLong()});
    return books;
}

void KnowledgeIndex::retake(const Library &library, const QList<Tenant> &tenants, const QSet<qint64> &things) {
    std::erase_if(m_entries, [&things](const Entry &entry) { return entry.node < 0 || things.contains(entry.node); });
    m_byBook.clear();
    m_taken.clear();
    for (const auto &[at, entry] : std::views::enumerate(m_entries)) {
        m_taken.insert({entry.node, entry.book}, static_cast<qsizetype>(at));
        m_byBook[entry.book].append(static_cast<qsizetype>(at));
    }
    QList<qint64> only = things.values();
    std::ranges::sort(only);
    const GraphKnown known = knownBy(library);
    for (const Tenant &tenant : tenants)
        take(Graph::forTenant(library, tenant), known, only);
}

void KnowledgeIndex::take(const Graph &graph, const GraphKnown &known, const std::optional<QList<qint64>> &only) {
    const QStringList kinds = KnowledgeIndex::kinds();
    const bool none = only && only->isEmpty();
    QList<Graph::Sighting> sightings = none ? QList<Graph::Sighting>() : graph.sightings(kinds, only);
    QHash<qint64, QList<GraphFact>> facts = sightings.isEmpty() ? QHash<qint64, QList<GraphFact>>() : graph.factsOfKinds(kinds, only);
    // And the graphs someone shared, as far as the reader has read each book.
    for (const GraphLayers::Layer &layer : GraphContext::layers(graph)) {
        sightings += layer.sightings();
        facts.insert(layer.facts);
    }
    sightings.removeIf([&kinds](const Graph::Sighting &sighting) { return !kinds.contains(sighting.node.kind); });
    for (const Graph::Sighting &sighting : std::as_const(sightings)) {
        if (sighting.bookId <= 0 || !known(sighting.at.place))
            continue;
        const auto which = std::pair{sighting.node.id, sighting.bookId};
        if (const auto before = m_taken.constFind(which); before != m_taken.cend()) {
            Entry &entry = m_entries[static_cast<std::size_t>(*before)];
            const GraphPlace &was = entry.met.place, &now = sighting.at.place;
            // A place in the book itself is worth more than none, and an earlier one more than a later.
            if (now.bookId == sighting.bookId && (was.bookId != sighting.bookId || !was.notAfter(now)))
                entry.met = sighting.at;
            continue;
        }
        const GraphNode node = Graph::asKnown(sighting.node, facts.value(sighting.node.id), sighting.bookId, known);
        Entry entry;
        entry.book = sighting.bookId;
        entry.node = node.id;
        entry.kind = static_cast<int>(kinds.indexOf(node.kind));
        entry.name = node.name;
        entry.names = (QStringList{node.name} + node.aliases()).join('\n').toCaseFolded();
        entry.said = node.summary.left(saidRoom).toCaseFolded();
        entry.met = sighting.at.place.bookId == sighting.bookId ? sighting.at : GraphMoment{};
        entry.from = GraphLayers::from(node);
        m_taken.insert(which, static_cast<qsizetype>(m_entries.size()));
        m_byBook[entry.book].append(static_cast<qsizetype>(m_entries.size()));
        m_entries.push_back(std::move(entry));
    }
}

QHash<qint64, KnownMatch> KnowledgeIndex::find(const QString &word, bool deep) const {
    QHash<qint64, KnownMatch> found;
    const QString wanted = word.simplified().toCaseFolded();
    if (wanted.size() < (deep ? 2 : 3))
        return found;
    const QStringList kinds = KnowledgeIndex::kinds();
    for (const Entry &entry : m_entries) {
        const bool byName = startsWordIn(entry.names, wanted);
        if (!byName && !(deep && startsWordIn(entry.said, wanted)))
            continue;
        // A thing found by its name says more than one found by what is said
        // of it, a kind that says more of a book more than another, and one's
        // own more than a shared graph's; of two alike, the one known first.
        const auto rank = [](bool inSummary, qsizetype kind, qint64 node) { return std::tuple{inSummary, kind, node < 0, node}; };
        if (const auto before = found.constFind(entry.book);
            before != found.cend()
            && rank(before->inSummary, kinds.indexOf(before->kind), before->node) <= rank(!byName, entry.kind, entry.node))
            continue;
        found.insert(entry.book, {.bookId = entry.book,
                                  .node = entry.node,
                                  .kind = kinds.value(entry.kind),
                                  .name = entry.name,
                                  .met = entry.met,
                                  .inSummary = !byName,
                                  .from = entry.from});
    }
    return found;
}

QList<KnownMatch> KnowledgeIndex::of(qint64 bookId, int most) const {
    QList<qsizetype> rows = m_byBook.value(bookId);
    // The kinds that say most of a book first; within a kind, the earliest met
    // (and of two met at once, one's own before a shared graph's, then the one
    // known first), whatever order they were read in.
    std::ranges::sort(rows, [this](qsizetype a, qsizetype b) {
        const Entry &x = m_entries[std::size_t(a)], &y = m_entries[std::size_t(b)];
        return std::tuple{x.kind, x.met.place.section, x.met.place.fraction, x.node < 0, x.node}
               < std::tuple{y.kind, y.met.place.section, y.met.place.fraction, y.node < 0, y.node};
    });
    QList<KnownMatch> known;
    const QStringList kinds = KnowledgeIndex::kinds();
    for (const qsizetype row : rows | std::views::take(std::max(0, most))) {
        const Entry &entry = m_entries[std::size_t(row)];
        known.append({.bookId = entry.book,
                      .node = entry.node,
                      .kind = kinds.value(entry.kind),
                      .name = entry.name,
                      .met = entry.met,
                      .inSummary = false,
                      .from = entry.from});
    }
    return known;
}

bool KnownWords::operator()(const Book &book, const QString &word, bool deep) {
    if (!hasIndex())
        return false;
    const QString key = (deep ? "deep:"_L1 : ":"_L1) + word;
    auto found = m_found.constFind(key);
    if (found == m_found.cend()) {
        if (!m_looking) {
            m_missed = true;
            return false;
        }
        found = m_found.insert(key, m_index->find(word, deep));
    }
    const auto match = found->constFind(book.id);
    if (match == found->cend())
        return false;
    if (!m_why.contains(book.id))
        m_why.insert(book.id, *match);
    return true;
}

std::optional<KnownMatch> KnownWords::why(qint64 bookId) const {
    const auto found = m_why.constFind(bookId);
    return found == m_why.cend() ? std::nullopt : std::optional(*found);
}

Book::Knows KnownWords::asker() {
    return [this](const Book &book, const QString &word, bool only) { return (*this)(book, word, only); };
}

Book::Lookups KnownWords::lookups(const QString &root) {
    return {.knows = hasIndex() ? asker() : Book::Knows(), .noted = AnnotationWords::noted(root)};
}

QList<KnowledgeSearch::Found> KnowledgeSearch::search(const Library &library, const QList<Book> &books, const QString &query,
                                                      const std::shared_ptr<const KnowledgeIndex> &index) {
    KnownWords words(index ? index : std::make_shared<const KnowledgeIndex>(KnowledgeIndex::forSearch(library, query)));
    const Book::Lookups lookups{.knows = words.asker(), .noted = AnnotationWords::noted(library)};
    QList<Found> found;
    for (const Book &book : books)
        if (book.matches(query, lookups))
            found.append({book, words.why(book.id)});
    return found;
}
