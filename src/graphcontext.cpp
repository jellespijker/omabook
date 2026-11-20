#include "graphcontext.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <functional>
#include <memory>


namespace {

constexpr qsizetype summaryRoom = 220;

QStringList namesOf(const GraphNode &node) { return QStringList{node.name} + node.aliases(); }

} // namespace

QString GraphContext::line(const GraphNode &node) {
    QString text = node.name + " (" + node.kind;
    if (!node.aliases().isEmpty())
        text += "; also " + node.aliases().join(", ");
    text += ")";
    if (!node.summary.isEmpty())
        text += ": " + node.summary.simplified().left(summaryRoom);
    return text;
}

namespace {

// Things one to a line, then how they relate, as the model reads them.
QString written(const QList<GraphNode> &nodes, const QList<GraphEdge> &edges, int mostEdges) {
    QHash<qint64, QString> names;
    QStringList lines;
    for (const GraphNode &node : nodes) {
        names.insert(node.id, node.name);
        lines << GraphContext::line(node);
    }
    int written = 0;
    for (const GraphEdge &edge : edges) {
        if (!names.contains(edge.source) || !names.contains(edge.target) || written >= mostEdges)
            continue;
        lines << names.value(edge.source) + " — " + edge.kind + " → " + names.value(edge.target)
                     + (edge.note.isEmpty() ? QString() : " (" + edge.note.left(120) + ")");
        ++written;
    }
    return lines.join('\n');
}

GraphView storyOf(const Graph &graph, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    return GraphContext::story(graph, bookId, upTo);
}

// The things of a view a question names, then what they are tied to: who a
// person belongs to, where a thing is.
QList<GraphNode> namedBy(const GraphView &view, const QString &lower, int mostNodes) {
    QList<GraphNode> chosen;
    QSet<qint64> taken;
    for (const GraphNode &node : view.nodes)
        if (chosen.size() < mostNodes && GraphContext::namedIn(node, lower, true)) {
            chosen << node;
            taken << node.id;
        }
    QSet<qint64> beside;
    for (const GraphEdge &edge : view.edges) {
        if (taken.contains(edge.source) && !taken.contains(edge.target))
            beside << edge.target;
        if (taken.contains(edge.target) && !taken.contains(edge.source))
            beside << edge.source;
    }
    for (const GraphNode &node : view.nodes)
        if (chosen.size() < mostNodes && beside.contains(node.id))
            chosen << node;
    return chosen;
}

// Things of these kinds, the most tied-in first, one to a line with their relations.
QString digestOf(GraphView view, const QStringList &kinds, int mostNodes) {
    view.nodes.removeIf([&](const GraphNode &node) { return !kinds.contains(node.kind); });
    // The most tied-in are the ones that matter; cut, they are kept.
    QHash<qint64, int> ties;
    for (const GraphEdge &edge : std::as_const(view.edges)) {
        ++ties[edge.source];
        ++ties[edge.target];
    }
    std::ranges::stable_sort(view.nodes, std::greater{}, [&](const GraphNode &node) { return ties.value(node.id); });
    return written(view.nodes.mid(0, mostNodes), view.edges, mostNodes * 2);
}

// What the graphs someone shared hold of a book, as far as its reader has
// read it (GraphContext::layers), each headed by whose it is so that the
// model tells it from the reader's own; `part` writes what of a layer's
// story goes, empty for none.
QString shared(const Graph &graph, qint64 bookId, const std::optional<GraphPlace> &upTo,
               const std::function<QString(const GraphView &)> &part) {
    QStringList parts;
    for (const GraphLayers::Layer &layer : GraphContext::layers(graph, upTo)) {
        GraphView view = layer.ofBook(bookId);
        view.nodes.removeIf([](const GraphNode &node) { return !GraphNode::storyKinds().contains(node.kind); });
        if (const QString text = view.nodes.isEmpty() ? QString() : part(view); !text.isEmpty())
            parts << "From " + GraphLayers::from(view.nodes.first(), GraphLayers::Said::English) + " (" + layer.name
                         + "), which someone shared: not the reader's own notes, and as far as the reader has read:\n" + text;
    }
    return parts.join("\n\n");
}

// The start of an answer, to its first sentence end past a line or so.
QString gistOf(const QString &answer) {
    const QString text = answer.simplified();
    if (text.size() <= 320)
        return text;
    qsizetype end = -1;
    for (const QChar mark : {QChar('.'), QChar('!'), QChar('?')}) {
        const qsizetype at = text.indexOf(mark, 120);
        if (at >= 0 && at < 320 && (end < 0 || at < end))
            end = at;
    }
    return end > 0 ? text.left(end + 1) : text.left(319) + "…";
}

} // namespace

QString GraphContext::brief(const QString &text, qsizetype room) {
    if (text.size() <= room)
        return text;
    const qsizetype end = qMax(text.lastIndexOf(". ", room), qMax(text.lastIndexOf("? ", room), text.lastIndexOf("! ", room)));
    return end > room / 3 ? text.left(end + 1) : text.left(room - 1) + "…";
}

namespace {

// The most connections across books that go with a question.
constexpr int mostConnections = 6;

} // namespace

GraphContext::Reach GraphContext::reachOf(const Library &library, qint64 bookId) {
    Reach reach;
    const auto book = library.book(bookId);
    if (!book)
        return reach;
    const Progress read = library.progress(bookId);
    reach.all = book->status() == "read";
    reach.place = furthest(library, bookId);
    // A place past the book's start is begun too, whatever percent was said
    // with it (`omabook progress --section` says none; a plugin's book in a
    // format not loaded has none worked out).
    reach.any = reach.all || read.percent > 0 || read.place.section > 0 || read.place.fraction > 0;
    return reach;
}

GraphPlace GraphContext::furthest(const Library &library, qint64 bookId) {
    const Progress read = library.progress(bookId);
    const GraphPlace here{bookId, read.place};
    // Kept by the database as the reading place moves (Library::open).
    const Recap most = library.notes(bookId, "furthest");
    const GraphPlace ever{bookId, most.place};
    return most.updated > 0 && here.notAfter(ever) ? ever : here;
}

std::optional<GraphPlace> GraphContext::spoilersAfter(const Library &library, qint64 bookId) {
    const auto book = library.book(bookId);
    if (!book || book->status() == "read" || book->percent <= 0)
        return std::nullopt;
    return furthest(library, bookId);
}

GraphKnown GraphContext::readerKnows(const Library &library) {
    auto reaches = std::make_shared<QHash<qint64, Reach>>();
    return [&library, reaches](const GraphPlace &at) {
        if (at.bookId == 0)
            return true;
        if (!reaches->contains(at.bookId))
            reaches->insert(at.bookId, reachOf(library, at.bookId));
        return reaches->value(at.bookId).covers(at);
    };
}

GraphKnown GraphContext::unspoiled(const Library &library) {
    auto bounds = std::make_shared<QHash<qint64, std::optional<GraphPlace>>>();
    return [&library, bounds](const GraphPlace &at) {
        if (at.bookId == 0)
            return true;
        if (!bounds->contains(at.bookId))
            bounds->insert(at.bookId, spoilersAfter(library, at.bookId));
        const std::optional<GraphPlace> bound = bounds->value(at.bookId);
        return !bound || at.notAfter(*bound);
    };
}

QList<GraphLayers::Layer> GraphContext::layers(const Graph &graph, const std::optional<GraphPlace> &upTo) {
    const Library &library = graph.library();
    const GraphKnown knows = readerKnows(library);
    // Of the books of the graph's tenant only: a layer is laid beside the
    // reader's own graph, and is as much of the tenant as it is.
    return GraphLayers::read(library, [&](const GraphPlace &at) {
        return graph.sees(at.bookId) && knows(at) && (!upTo || at.bookId != upTo->bookId || at.notAfter(*upTo));
    });
}

GraphView GraphContext::story(const Graph &graph, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    GraphView view = graph.ofBook(bookId, upTo);
    view.nodes.removeIf([](const GraphNode &node) { return !GraphNode::storyKinds().contains(node.kind); });
    QSet<qint64> kept;
    for (const GraphNode &node : std::as_const(view.nodes))
        kept << node.id;
    view.edges.removeIf([&](const GraphEdge &edge) { return !kept.contains(edge.source) || !kept.contains(edge.target); });
    return view;
}

QString GraphContext::bookOf(const Graph &graph, qint64 node, qint64 notBook) {
    for (const GraphPlace &place : graph.seen(node))
        if (place.bookId != notBook)
            if (const auto book = graph.library().book(place.bookId))
                return book->meta.title;
    return {};
}

// Names of a letter or two would be found in every other word, and so
// would the short words of a longer name.
bool GraphContext::namedIn(const GraphNode &node, const QString &lower, bool loosely) {
    static const QRegularExpression apart("[^\\p{L}\\p{N}]+");
    const QStringList names = namesOf(node);
    return std::ranges::any_of(names, [&](const QString &name) {
        if (name.size() >= 3 && lower.contains(name.toLower()))
            return true;
        if (!loosely)
            return false;
        const QStringList words = name.toLower().split(apart, Qt::SkipEmptyParts);
        return std::ranges::any_of(words, [&](const QString &word) {
            return word.size() >= 4 && lower.contains(word);
        });
    });
}

QString GraphContext::forQuestion(const Graph &graph, qint64 bookId, const GraphPlace &upTo, const QString &words, int mostNodes) {
    const GraphView view = storyOf(graph, bookId, upTo);
    const QString lower = words.toLower();
    QString theirs = shared(graph, bookId, upTo, [&lower, mostNodes](const GraphView &layer) {
        return written(namedBy(layer, lower, mostNodes), layer.edges, mostNodes * 2);
    });
    const QList<GraphNode> chosen = namedBy(view, lower, mostNodes);
    if (chosen.isEmpty())
        return theirs;
    QString slice = written(chosen, view.edges, mostNodes * 2);
    // And what those things connect to in the other books read: a few
    // lines, each naming the book, so the answer can say "you met a
    // similar idea in ...".
    const GraphView beyond = graph.beyond(bookId, upTo);
    QHash<qint64, GraphNode> ends;
    for (const GraphNode &node : chosen + beyond.nodes)
        ends.insert(node.id, node);
    QSet<qint64> asked;
    for (const GraphNode &node : std::as_const(chosen))
        asked << node.id;
    int connections = 0;
    for (const GraphEdge &edge : beyond.edges) {
        if (connections >= mostConnections)
            break;
        const bool outward = asked.contains(edge.source);
        if (!edge.across() || (!outward && !asked.contains(edge.target)))
            continue;
        const GraphNode other = ends.value(outward ? edge.target : edge.source);
        const QString title = bookOf(graph, other.id, bookId);
        if (other.id == 0 || title.isEmpty())
            continue;
        slice += "\n" + ends.value(edge.source).name + " — " + edge.relation() + " → " + ends.value(edge.target).name + " [" + other.kind
                 + " in " + title + (other.summary.isEmpty() ? QString() : ": " + brief(other.summary, 140)) + "]"
                 + (edge.note.isEmpty() ? QString() : " (" + brief(edge.note, 160) + ")");
        ++connections;
    }
    return theirs.isEmpty() ? slice : slice + "\n\n" + theirs;
}

QString GraphContext::digest(const Graph &graph, qint64 bookId, const GraphPlace &upTo, const QStringList &kinds, int mostNodes) {
    const QString own = digestOf(storyOf(graph, bookId, upTo), kinds, mostNodes);
    const QString theirs =
        shared(graph, bookId, upTo, [&kinds, mostNodes](const GraphView &layer) { return digestOf(layer, kinds, mostNodes); });
    return own.isEmpty() || theirs.isEmpty() ? own + theirs : own + "\n\n" + theirs;
}

namespace {

// What a question is called: its words, and of which book, or in which
// tenant, when the same words were asked of another or in another.
QString questionName(const Graph &graph, const std::optional<Book> &book, const QString &question) {
    QString name = question.simplified().left(140);
    const qint64 before = name.isEmpty() ? 0 : graph.named({}, "question", name);
    if (before == 0)
        return name;
    if (const std::optional<Tenant> &tenant = graph.tenant(); tenant && !graph.node(before))
        return name + " (" + Tenancy::nameOf(graph.library(), *tenant) + ")";
    if (Graph(graph.library()).node(before).value_or(GraphNode()).data.value("book").toInteger() != (book ? book->id : 0))
        return name + " (" + (book ? book->meta.title : QString("the library")) + ")";
    return name;
}

// A question asked of a book: tied to it, and to the things of the book it
// and its answer name, as far as the reader had got.
void mentionedInBook(Graph &graph, qint64 id, qint64 bookId, const std::optional<GraphPlace> &at, const QString &said) {
    graph.link(id, graph.bookNode(bookId), "asked about", {}, at);
    int mentions = 0;
    for (const GraphNode &node : storyOf(graph, bookId, at).nodes)
        if (mentions < 12 && GraphContext::namedIn(node, said)) {
            graph.link(id, node.id, "mentions", {}, at);
            ++mentions;
        }
}

// A question asked of the library: the books of the tenant the answer
// names, and the themes, ideas and topics the question does.
void mentionedInLibrary(Graph &graph, qint64 id, const QString &question, const QString &said) {
    int mentions = 0;
    for (const Book &other : graph.library().books())
        if (mentions < 12 && graph.sees(other.id) && other.meta.title.size() >= 4 && said.contains(other.meta.title.toLower())) {
            graph.link(id, graph.bookNode(other.id), "mentions");
            ++mentions;
        }
    const QStringList words = question.toLower().split(QRegularExpression("[^\\p{L}\\p{N}]+"), Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word.size() < 4)
            continue;
        for (const GraphNode &node : graph.find(word, {}, QString(), 8))
            if (mentions < 20 && GraphNode::storyKinds().contains(node.kind) && GraphContext::namedIn(node, said)) {
                graph.link(id, node.id, "mentions");
                ++mentions;
            }
    }
}

} // namespace

qint64 GraphContext::noteQuestion(const Graph &graph, qint64 bookId, const std::optional<GraphPlace> &place, const QString &question,
                                  const QString &answer) {
    // Written as the tenant it was asked in reads it, and kept there (Graph::upsert).
    Graph writing = graph;
    const auto book = bookId > 0 ? writing.library().book(bookId) : std::nullopt;
    const QString name = questionName(writing, book, question);
    if (name.isEmpty() || (bookId > 0 && !book))
        return 0;
    QJsonObject data{{"asked", QDateTime::currentDateTime().toString(Qt::ISODate)}, {"of", book ? "book" : "library"}};
    if (book)
        data.insert("book", bookId);
    const std::optional<GraphPlace> at = book && place ? std::optional(GraphPlace{bookId, place->section, place->fraction}) : std::nullopt;
    const qint64 id = writing.upsert({}, "question", name, gistOf(answer), data, at);
    if (id == 0)
        return 0;
    const QString said = (question + "\n" + answer).toLower();
    if (book)
        mentionedInBook(writing, id, bookId, at, said);
    else
        mentionedInLibrary(writing, id, question, said);
    return id;
}
