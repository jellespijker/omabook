// The GraphOps that read the knowledge graph (graphtools.h), each through a
// tenant's graph and so of that tenant alone (ADR 0003, #95): a book's graph
// or a whole scope, a search, a thing and what is around it, the graph as a
// file, the scopes and the reading log. What grows and changes it, and the
// command line and the MCP tools, are in graphtools.cpp.

#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <algorithm>

#include "graphcontext.h"
#include "graphcut.h"
#include "graphtools.h"
#include "graphwalk.h"
#include "tenancy.h"

namespace {

QJsonObject problem(const QString &why) { return {{"error", why}}; }

QJsonObject placeJson(const GraphPlace &place) {
    return {{"book", place.bookId}, {"section", place.section}, {"fraction", place.fraction}};
}

// The tenant an answer was read in: {"id", "name"}.
QJsonObject tenantJson(const Graph &graph) {
    if (!graph.tenant())
        return {};
    return {{"id", graph.tenant()->globalId()}, {"name", Tenancy::nameOf(graph.library(), *graph.tenant())}};
}

// Why a book can't be read in the graph's tenant; nothing when it can.
std::optional<QJsonObject> refused(const Graph &graph, qint64 bookId) {
    if (bookId <= 0)
        return std::nullopt;
    if (!graph.library().book(bookId))
        return problem(QString("There is no book %1 in the library.").arg(bookId));
    if (!graph.sees(bookId))
        return problem(
            QString("Book %1 is not in %2.").arg(bookId).arg(Tenancy::nameOf(graph.library(), graph.tenant().value_or(Tenant::unfiled()))));
    return std::nullopt;
}

// Makes sure the books stand in the graph before a whole scope is shown.
// Bringing every book up to date is slow for a large library, so it is only
// done when books came or went since the last time; `omabook graph sync`
// does it whenever asked.
void syncWhenBooksChanged(const Library &library) {
    Graph graph(library);
    if (graph.find({}, "book", QString(), 1000000).size() != library.books().size())
        graph.syncLibrary();
}

// What a book connects to elsewhere, with the books of the tenant each thing is in.
QJsonObject beyondJson(const Graph &graph, const GraphView &view) {
    QJsonArray nodes;
    for (const GraphNode &node : view.nodes) {
        QJsonObject object = node.toJson();
        QJsonArray in;
        for (const GraphPlace &place : graph.seen(node.id))
            if (const auto book = graph.library().book(place.bookId))
                in << QJsonObject{{"book", book->id}, {"title", book->meta.title}};
        if (!in.isEmpty())
            object.insert("in", in);
        nodes << object;
    }
    QJsonObject answer = view.toJson();
    answer.insert("nodes", nodes);
    return answer;
}

// Whether a place of a book is past where a reader of it at `upTo` is.
bool past(const GraphPlace &at, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    return bookId > 0 && upTo && at.bookId == bookId && !at.notAfter(*upTo);
}

// Where a thing was met, each place with its book's title, up to a place of a book.
QJsonArray seenJson(const Graph &graph, qint64 id, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    QJsonArray seen;
    for (const GraphPlace &place : graph.seen(id)) {
        if (past(place, bookId, upTo))
            continue;
        QJsonObject at = placeJson(place);
        if (const auto book = graph.library().book(place.bookId))
            at.insert("title", book->meta.title);
        seen << at;
    }
    return seen;
}

// When each thing known of a thing was revealed, up to a place of a book,
// what is past its reader's furthest place marked "ahead".
QJsonArray timelineJson(const Graph &graph, qint64 id, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    const GraphKnown unspoiled = GraphContext::unspoiled(graph.library());
    QJsonArray steps;
    for (const GraphReveal &step : graph.timeline(id, bookId)) {
        const GraphPlace &at = step.revealed.place;
        if (past(at, bookId, upTo))
            continue;
        QJsonObject object = step.toJson();
        if (const auto book = at.bookId > 0 ? graph.library().book(at.bookId) : std::nullopt)
            object.insert("title", book->meta.title);
        if (!unspoiled(at))
            object.insert("ahead", true);
        steps << object;
    }
    return steps;
}

} // namespace

QJsonObject GraphOps::show(const Graph &graph, qint64 bookId, const QString &scope, const std::optional<GraphPlace> &upTo, int limit,
                           bool beyond) {
    if (const auto why = refused(graph, bookId))
        return *why;
    if (bookId == 0 && scope.isEmpty())
        syncWhenBooksChanged(graph.library());
    QJsonObject answer = bookId > 0 ? graph.ofBook(bookId, upTo).toJson() : GraphCut::whole(graph, scope, qMax(1, limit)).toJson();
    if (bookId > 0) {
        answer.insert("book", bookId);
        if (const auto built = GraphBuild::builtTo(graph.library(), bookId))
            answer.insert("built_to", placeJson(*built));
        if (beyond)
            answer.insert("beyond", beyondJson(graph, graph.beyond(bookId, upTo)));
        // The graphs someone shared, apart, as far as the reader has read the book.
        if (const QList<GraphLayers::Layer> layers = GraphContext::layers(graph, upTo); !layers.isEmpty())
            answer.insert("layers", GraphLayers::toJson(layers, bookId));
    } else {
        answer.insert("scope", scope);
    }
    answer.insert("tenant", tenantJson(graph));
    return answer;
}

QJsonObject GraphOps::find(const Graph &graph, const QString &words, const QString &kind, const QString &scope, bool everywhere,
                           int limit) {
    QJsonArray nodes;
    for (const GraphNode &node : graph.find(words, kind, everywhere ? std::nullopt : std::optional(scope), qMax(1, limit)))
        nodes << node.toJson();
    return {{"nodes", nodes}, {"tenant", tenantJson(graph)}};
}

QJsonObject GraphOps::node(const Graph &graph, qint64 id, int depth, bool timeline, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    auto found = graph.node(id);
    if (!found)
        return problem(QString("There is no node %1 in the graph.").arg(id));
    if (const auto why = refused(graph, bookId))
        return *why;
    const bool bounded = bookId > 0 && upTo;
    // As a reader at that place knows it, among what they have met.
    GraphCut around;
    if (bounded) {
        const GraphView book = graph.ofBook(bookId, upTo);
        const auto self = std::ranges::find(book.nodes, id, &GraphNode::id);
        if (self == book.nodes.end())
            return problem(QString("Node %1 is not met in book %2 by that place.").arg(id).arg(bookId));
        found = *self;
        around = GraphCut(GraphWalk::within(book, id, qBound(0, depth, 6)));
        // The node asked about first.
        std::ranges::stable_partition(around.view.nodes, [id](const GraphNode &node) { return node.id == id; });
    } else {
        around = GraphCut::around(graph, id, depth, 200);
    }
    QJsonObject answer = around.toJson();
    answer.insert("node", found->toJson());
    answer.insert("seen", seenJson(graph, id, bookId, upTo));
    if (timeline)
        answer.insert("timeline", timelineJson(graph, id, bookId, upTo));
    answer.insert("tenant", tenantJson(graph));
    return answer;
}

QJsonObject GraphOps::exported(const Graph &graph, qint64 bookId, const QString &scope, const std::optional<GraphPlace> &upTo,
                               const QString &format, const QString &output) {
    if (!GraphView::formats().contains(format))
        return problem("The graph is written as " + GraphView::formats().join(", ") + ", not as \"" + format + "\".");
    if (const auto why = refused(graph, bookId))
        return *why;
    if (bookId == 0 && scope.isEmpty())
        syncWhenBooksChanged(graph.library());
    const GraphView view = bookId > 0 ? graph.ofBook(bookId, upTo) : graph.whole(scope, 1000000);
    const QByteArray content = view.exported(format);
    QJsonObject answer{{"format", format}, {"nodes", view.nodes.size()}, {"edges", view.edges.size()}, {"tenant", tenantJson(graph)}};
    if (output.isEmpty()) {
        answer.insert("content", QString::fromUtf8(content));
        return answer;
    }
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
        return problem("The graph can't be written to " + output + ".");
    answer.insert("file", QFileInfo(output).absoluteFilePath());
    return answer;
}

QJsonObject GraphOps::scopes(const Graph &graph) {
    QJsonArray list;
    for (const QString &scope : graph.scopes())
        list << QJsonObject{{"scope", scope}, {"nodes", GraphCut::whole(graph, scope, 1).things}};
    return {{"scopes", list}, {"tenant", tenantJson(graph)}};
}

QJsonObject GraphOps::log(const Graph &graph, qint64 bookId) {
    if (const auto why = refused(graph, bookId))
        return *why;
    QHash<qint64, QString> titles;
    QJsonArray days;
    for (const ReadingDay &day : graph.readingLog(bookId)) {
        if (!titles.contains(day.bookId)) {
            const auto book = graph.library().book(day.bookId);
            titles.insert(day.bookId, book ? book->meta.title : QString());
        }
        days << QJsonObject{
            {"book", day.bookId}, {"title", titles.value(day.bookId)}, {"day", day.day}, {"from", day.from}, {"to", day.to}};
    }
    return {{"days", days}, {"tenant", tenantJson(graph)}};
}
