#include "graphlinks.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <optional>
#include <utility>

#include "graphcontext.h"
#include "prompts.h"

namespace {

constexpr qsizetype summaryRoom = 140;

// The words of a thing that say something: its name's and its summary's,
// lower case, without the short ones every sentence has.
QSet<QString> wordsOf(const GraphNode &node) {
    static const QRegularExpression apart("[^\\p{L}\\p{N}]+");
    QSet<QString> words;
    const QStringList all = (node.name + " " + node.aliases().join(' ') + " " + node.summary).toLower().split(apart, Qt::SkipEmptyParts);
    for (const QString &word : all)
        if (word.size() >= 5)
            words << word;
    return words;
}

// The nodes of one kind, the most tied-in first.
QList<GraphNode> pick(const GraphView &view, const QHash<qint64, int> &ties, const QString &kind, int most) {
    QList<GraphNode> nodes;
    for (const GraphNode &node : view.nodes)
        if (node.kind == kind)
            nodes << node;
    std::ranges::stable_sort(nodes, std::greater{}, [&](const GraphNode &node) { return ties.value(node.id); });
    return nodes.mid(0, most);
}

QHash<qint64, int> tiesOf(const GraphView &view) {
    QHash<qint64, int> ties;
    for (const GraphEdge &edge : view.edges) {
        ++ties[edge.source];
        ++ties[edge.target];
    }
    return ties;
}

QString line(const GraphNode &node, const QString &title = {}) {
    return QString("#%1 %2 (%3%4)%5").arg(node.id).arg(node.name, node.kind, title.isEmpty() ? QString() : "; in " + title,
                                                      node.summary.isEmpty() ? QString() : ": " + node.summary.simplified().left(summaryRoom));
}

// A number as a model writes it: 12, "12" or "#12".
qint64 numberOf(const QJsonValue &value) {
    if (value.isDouble())
        return value.toInteger();
    QString text = value.toString().trimmed();
    if (text.startsWith('#'))
        text.remove(0, 1);
    return text.toLongLong();
}

QPair<qint64, qint64> pairOf(qint64 a, qint64 b) { return {qMin(a, b), qMax(a, b)}; }

// The pairs of things a connection across books already ties, whichever way.
QSet<QPair<qint64, qint64>> connected(const Graph &graph, qint64 bookId) {
    QSet<QPair<qint64, qint64>> pairs;
    for (const GraphEdge &edge : graph.beyond(bookId).edges)
        if (edge.across())
            pairs << pairOf(edge.source, edge.target);
    for (const GraphEdge &edge : graph.ofBook(bookId).edges)
        if (edge.across())
            pairs << pairOf(edge.source, edge.target);
    return pairs;
}

const QString linksNote = "graphlinks";

} // namespace

QString GraphLinks::Offer::mark() const {
    QStringList ids;
    for (const GraphNode &node : own)
        ids << QString::number(node.id);
    ids << "|";
    for (const GraphNode &node : others)
        ids << QString::number(node.id);
    return QString::fromLatin1(QCryptographicHash::hash(ids.join(',').toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

GraphLinks::Offer GraphLinks::offer(const Library &library, qint64 bookId, const GraphPlace &upTo) {
    Offer offer;
    const auto book = library.book(bookId);
    if (!book)
        return offer;
    const Graph graph(library);
    const GraphView mine = GraphContext::story(library, bookId, upTo);
    const QHash<qint64, int> ties = tiesOf(mine);
    // What a book is about and how its world works say most; then what it
    // leaves open, and who carries it.
    offer.own = pick(mine, ties, "theme", 10) + pick(mine, ties, "concept", 8) + pick(mine, ties, "thread", 6)
                + pick(mine, ties, "character", 6);
    if (offer.own.isEmpty())
        return offer;
    QSet<qint64> here;
    for (const GraphNode &node : mine.nodes)
        here << node.id;
    QSet<QString> ownWords;
    for (const GraphNode &node : std::as_const(offer.own))
        ownWords += wordsOf(node);

    // The other books that have a graph their reader has reached, the
    // nearest to this one first: the same author or series, shared tags,
    // and failing that the ones read last. Only those on its side of the
    // library: a book on another is not even shown to the model.
    struct Near {
        Book book;
        int score = 0;
    };
    QList<Near> near;
    for (const Book &other : library.books()) {
        if (other.id == bookId || !graph.sides().together(bookId, other.id) || !GraphBuild::builtTo(library, other.id)
            || !GraphContext::reachOf(library, other.id).any)
            continue;
        int score = 0;
        for (const QString &author : other.meta.authors)
            if (book->meta.authors.contains(author, Qt::CaseInsensitive))
                score = 3;
        if (!book->meta.series.trimmed().isEmpty() && other.meta.series.trimmed().compare(book->meta.series.trimmed(), Qt::CaseInsensitive) == 0)
            score += 3;
        int tags = 0;
        for (const QString &tag : other.meta.tags)
            if (book->meta.tags.contains(tag, Qt::CaseInsensitive))
                ++tags;
        near.append({other, score + qMin(3, tags)});
    }
    std::ranges::stable_sort(near, [](const Near &a, const Near &b) {
        return a.score != b.score ? a.score > b.score : a.book.lastRead > b.book.lastRead;
    });

    struct Candidate {
        GraphNode node;
        QString title;
        qint64 bookId = 0;
        int score = 0;
    };
    QList<Candidate> candidates;
    QSet<qint64> taken;
    for (const Near &other : near.mid(0, mostBooks)) {
        const GraphView theirs = GraphContext::story(library, other.book.id, GraphContext::reachOf(library, other.book.id).upTo());
        const QHash<qint64, int> theirTies = tiesOf(theirs);
        const QList<GraphNode> telling = pick(theirs, theirTies, "theme", 10) + pick(theirs, theirTies, "concept", 8)
                                         + pick(theirs, theirTies, "thread", 5) + pick(theirs, theirTies, "topic", 5)
                                         + pick(theirs, theirTies, "character", 4);
        for (const GraphNode &node : telling) {
            // A thing both books have is one node already: nothing to link.
            if (here.contains(node.id) || taken.contains(node.id))
                continue;
            taken << node.id;
            int shared = 0;
            for (const QString &word : wordsOf(node))
                if (ownWords.contains(word))
                    ++shared;
            candidates.append({node, other.book.meta.title, other.book.id,
                               other.score + 2 * qMin(4, shared) + (node.kind == "theme" || node.kind == "concept" ? 2 : 0)});
        }
    }
    std::ranges::stable_sort(candidates, std::greater{}, &Candidate::score);
    QHash<qint64, int> fromBook;
    for (const Candidate &candidate : std::as_const(candidates)) {
        if (offer.others.size() >= mostOthers)
            break;
        if (fromBook[candidate.bookId] >= mostFromOneBook)
            continue;
        ++fromBook[candidate.bookId];
        offer.others << candidate.node;
        offer.titles.insert(candidate.node.id, candidate.title);
    }

    QStringList known;
    for (const QPair<qint64, qint64> &pair : connected(graph, bookId))
        known << QString("#%1 and #%2").arg(pair.first).arg(pair.second);
    known.sort();
    offer.already = known.mid(0, 60).join('\n');
    return offer;
}

AiRequest GraphLinks::request(const Library &library, qint64 bookId, const Offer &offer) {
    QStringList own, others;
    for (const GraphNode &node : offer.own)
        own << line(node);
    for (const GraphNode &node : offer.others)
        others << line(node, offer.titles.value(node.id));
    const auto book = library.book(bookId);
    return Prompts::graphConnections(book ? book->meta : BookMeta(), own.join('\n'), others.join('\n'), offer.already);
}

GraphLinks::Applied GraphLinks::apply(Library &library, qint64 bookId, const GraphPlace &at, const Offer &offer, const QString &answer) {
    Applied applied;
    const qsizetype start = answer.indexOf('{'), end = answer.lastIndexOf('}');
    if (start < 0 || end <= start || !library.book(bookId))
        return applied;
    QJsonParseError error;
    const QJsonObject object = QJsonDocument::fromJson(answer.mid(start, end - start + 1).toUtf8(), &error).object();
    if (error.error != QJsonParseError::NoError || (!object.value("links").isArray() && !object.value("topics").isArray()))
        return applied;
    applied.understood = true;

    const Library::Batch batch(library);
    Graph graph(library);
    QSet<qint64> own, others;
    for (const GraphNode &node : offer.own)
        own << node.id;
    for (const GraphNode &node : offer.others)
        others << node.id;
    QSet<QPair<qint64, qint64>> pairs = connected(graph, bookId);
    const GraphPlace place{bookId, at.section, at.fraction};
    // The two ends as offered, whichever way round the model put them;
    // anything else it named was not on offer and is not linked.
    const auto ends = [&own, &others](const QJsonObject &entry) -> std::optional<std::pair<qint64, qint64>> {
        qint64 from = numberOf(entry.value("from")), to = numberOf(entry.value("to"));
        if (own.contains(to) && others.contains(from))
            std::swap(from, to);
        if (!own.contains(from) || !others.contains(to))
            return std::nullopt;
        return std::pair{from, to};
    };

    const QJsonArray links = object.value("links").toArray();
    for (const QJsonValue &offered : links) {
        if (applied.links >= mostLinks)
            break;
        const QJsonObject link = offered.toObject();
        const auto linked = ends(link);
        if (!linked || pairs.contains(pairOf(linked->first, linked->second)))
            continue;
        const auto [from, to] = *linked;
        QString relation = link.value("relation").toString().simplified().toLower();
        if (relation.startsWith(GraphEdge::acrossMark()))
            relation.remove(0, GraphEdge::acrossMark().size());
        if (relation.isEmpty())
            relation = "echoes";
        if (graph.link(from, to, GraphEdge::acrossMark() + relation.left(40), link.value("note").toString().simplified().left(300), place) > 0) {
            pairs << pairOf(from, to);
            ++applied.links;
        }
    }

    const QJsonArray topics = object.value("topics").toArray();
    for (const QJsonValue &offered : topics) {
        if (applied.topics >= mostTopics)
            break;
        const QJsonObject topic = offered.toObject();
        const QString name = topic.value("name").toString().simplified();
        const auto linked = ends(topic);
        if (name.isEmpty() || name.size() > 120 || !linked)
            continue;
        const auto [from, to] = *linked;
        // What the work is stays as it was first put, as for any topic.
        const qint64 before = graph.named({}, "topic", name);
        const bool keep = before > 0 && !graph.node(before)->summary.isEmpty();
        const qint64 id = graph.upsert({}, "topic", name, keep ? QString() : topic.value("summary").toString().simplified().left(600), {}, place);
        if (id == 0 || (pairs.contains(pairOf(from, id)) && pairs.contains(pairOf(to, id))))
            continue;
        const QString note = topic.value("note").toString().simplified().left(300);
        graph.link(from, id, GraphEdge::acrossMark() + "refers to", note, place);
        graph.link(to, id, GraphEdge::acrossMark() + "refers to", note, place);
        graph.link(id, graph.bookNode(bookId), "referred to in", {}, place);
        pairs << pairOf(from, id) << pairOf(to, id);
        ++applied.topics;
    }
    return applied;
}

void GraphLinks::setConnected(Library &library, qint64 bookId, const GraphPlace &at, const Offer &offer) {
    library.setNotes(bookId, linksNote, {at.section, at.fraction, offer.mark(), 0});
}

bool GraphLinks::due(const Library &library, qint64 bookId) {
    const auto at = GraphBuild::builtTo(library, bookId);
    if (!at)
        return false;
    const Offer made = offer(library, bookId, *at);
    return !made.empty() && library.notes(bookId, linksNote).text != made.mark();
}

bool GraphLinks::company(const Library &library, qint64 bookId) {
    const BookSides sides = BookSides::of(library);
    for (const Book &other : library.books())
        if (other.id != bookId && sides.together(bookId, other.id) && GraphBuild::builtTo(library, other.id)
            && GraphContext::reachOf(library, other.id).any)
            return true;
    return false;
}

GraphLinks::Connected GraphLinks::connect(Library &library, qint64 bookId, const GraphBuild::Asker &ask, bool again) {
    Connected connected;
    const auto at = GraphBuild::builtTo(library, bookId);
    if (!library.book(bookId) || !at) {
        connected.skipped = "This book has no graph yet to connect; build it first.";
        return connected;
    }
    const Offer made = offer(library, bookId, *at);
    if (made.empty()) {
        connected.skipped = made.own.isEmpty()                ? QString("Its graph holds no themes, ideas, threads or characters to connect yet.")
                            : BookSides::of(library).apart() ? QString("No other book in its top collection has a graph, as far as it was "
                                                                       "read, to connect it with.")
                                                             : QString("No other book has a graph, as far as it was read, to connect it with.");
        return connected;
    }
    if (!again && library.notes(bookId, linksNote).text == made.mark()) {
        connected.skipped = "It was already compared with the same things of the other books.";
        return connected;
    }
    QString answer;
    if (!ask(request(library, bookId, made), &answer, &connected.problem))
        return connected;
    connected.asked = true;
    const Applied applied = apply(library, bookId, *at, made, answer);
    if (!applied.understood)
        connected.problem = "The model's answer was not the JSON asked for, so no connections were made.";
    connected.links = applied.links;
    connected.topics = applied.topics;
    // Kept either way: asking the same again would cost the same.
    setConnected(library, bookId, *at, made);
    return connected;
}
