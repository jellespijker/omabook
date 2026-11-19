#include "knowledgesearch.h"

#include <QSet>
#include <algorithm>
#include <ranges>
#include <tuple>

#include "graphcontext.h"

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

KnowledgeIndex KnowledgeIndex::build(const Library &library) {
    KnowledgeIndex index;
    index.changes = library.changes();
    const QStringList kinds = KnowledgeIndex::kinds();
    const Graph graph(library);
    QList<Graph::Sighting> sightings = graph.sightings(kinds);
    QHash<qint64, QList<GraphFact>> facts = sightings.isEmpty() ? QHash<qint64, QList<GraphFact>>() : graph.factsOfKinds(kinds);
    // And the graphs someone shared, as far as the reader has read each book.
    for (const GraphLayers::Layer &layer : GraphContext::layers(library)) {
        sightings += layer.sightings();
        facts.insert(layer.facts);
    }
    sightings.removeIf([&kinds](const Graph::Sighting &sighting) { return !kinds.contains(sighting.node.kind); });
    if (sightings.isEmpty())
        return index;

    // How far each book was read, asked once per book.
    QHash<qint64, GraphContext::Reach> reaches;
    const auto reach = [&](qint64 bookId) -> const GraphContext::Reach & {
        auto found = reaches.find(bookId);
        if (found == reaches.end())
            found = reaches.insert(bookId, GraphContext::reachOf(library, bookId));
        return *found;
    };
    const GraphKnown known = [&reach](const GraphPlace &at) { return at.bookId == 0 || reach(at.bookId).covers(at); };

    // One entry per thing and book, at the earliest place it is known from.
    QHash<std::pair<qint64, qint64>, qsizetype> taken;
    for (const Graph::Sighting &sighting : sightings) {
        if (sighting.bookId <= 0 || !known(sighting.at.place))
            continue;
        const auto key = std::pair{sighting.node.id, sighting.bookId};
        if (const auto before = taken.constFind(key); before != taken.cend()) {
            Entry &entry = index.m_entries[std::size_t(*before)];
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
        entry.kind = int(kinds.indexOf(node.kind));
        entry.name = node.name;
        entry.names = (QStringList{node.name} + node.aliases()).join('\n').toCaseFolded();
        entry.said = node.summary.left(saidRoom).toCaseFolded();
        entry.met = sighting.at.place.bookId == sighting.bookId ? sighting.at : GraphMoment{};
        entry.from = GraphLayers::from(node);
        taken.insert(key, qsizetype(index.m_entries.size()));
        index.m_byBook[entry.book].append(qsizetype(index.m_entries.size()));
        index.m_entries.push_back(std::move(entry));
    }
    return index;
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
        // of it, and a kind that says more of a book more than another.
        if (const auto before = found.constFind(entry.book);
            before != found.cend() && std::tuple{before->inSummary, kinds.indexOf(before->kind)} <= std::tuple{!byName, qsizetype(entry.kind)})
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
    // The kinds that say most of a book first; within a kind, the earliest met.
    std::ranges::stable_sort(rows, [this](qsizetype a, qsizetype b) {
        const Entry &x = m_entries[std::size_t(a)], &y = m_entries[std::size_t(b)];
        return std::tuple{x.kind, x.met.place.section, x.met.place.fraction} < std::tuple{y.kind, y.met.place.section, y.met.place.fraction};
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

QList<KnowledgeSearch::Found> KnowledgeSearch::search(const Library &library, const QList<Book> &books, const QString &query,
                                                      const std::shared_ptr<const KnowledgeIndex> &index) {
    KnownWords words(index ? index : std::make_shared<const KnowledgeIndex>(KnowledgeIndex::build(library)));
    const Book::Knows knows = words.asker();
    QList<Found> found;
    for (const Book &book : books)
        if (book.matches(query, knows))
            found.append({book, words.why(book.id)});
    return found;
}
