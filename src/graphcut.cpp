#include "graphcut.h"

#include <QHash>
#include <QSqlQuery>
#include <algorithm>
#include <limits>
#include <utility>

namespace {

[[nodiscard]] qint64 countOf(QSqlQuery query) { return query.next() ? query.value(0).toLongLong() : 0; }

} // namespace

// A connection across books between two sides that don't meet is never
// answered (Graph::sides), so where sides are apart those connections are
// counted as Graph answers them.
qint64 GraphCut::relationsIn(const Library &library, const Graph &graph, const QString &scope) {
    const bool apart = graph.sides().apart();
    qint64 count = countOf(library.query(QString("SELECT COUNT(*) FROM graph_edges JOIN graph_nodes AS source_node ON "
                                                 "source_node.id = source JOIN graph_nodes AS target_node ON target_node.id = "
                                                 "target WHERE source_node.scope = ? AND target_node.scope = ?%1")
                                             .arg(apart ? " AND graph_edges.kind NOT LIKE 'across: %'" : ""),
                                         {scope, scope}));
    if (apart) {
        const GraphView across = graph.acrossBooks(std::numeric_limits<int>::max());
        QHash<qint64, QString> scopes;
        for (const GraphNode &node : across.nodes)
            scopes.insert(node.id, node.scope);
        count += std::ranges::count_if(
            across.edges, [&](const GraphEdge &edge) { return scopes.value(edge.source) == scope && scopes.value(edge.target) == scope; });
    }
    return count;
}

GraphCut::GraphCut(GraphView whole) : view(std::move(whole)), things(view.nodes.size()), relations(view.edges.size()) {}

QJsonObject GraphCut::toJson() const {
    QJsonObject answer = view.toJson();
    answer.insert("total", QJsonObject{{"nodes", things}, {"edges", relations}});
    if (cut())
        answer.insert("cut", by == By::Nearest ? "nearest" : "most connected");
    return answer;
}

GraphCut GraphCut::whole(const Library &library, const QString &scope, int limit) {
    const Graph graph(library);
    const QString in = scope.isNull() ? QString("") : scope;
    GraphCut made(graph.whole(in, limit));
    // Only a view that came to its limit can have left something out.
    if (made.view.nodes.size() < limit)
        return made;
    made.things = std::max(made.things, countOf(library.query("SELECT COUNT(*) FROM graph_nodes WHERE scope = ?", {in})));
    if (made.cut())
        made.relations = std::max(made.relations, relationsIn(library, graph, in));
    return made;
}

GraphCut GraphCut::around(const Library &library, qint64 node, int depth, int limit) {
    const Graph graph(library);
    GraphCut made(graph.around(node, depth, limit));
    made.by = By::Nearest;
    if (made.view.nodes.size() < limit)
        return made;
    // Everything within `depth` relations, walked in the storage both ways
    // along each relation, and the relations among what was reached.
    const QString along = graph.sides().apart() ? " AND kind NOT LIKE 'across: %'" : "";
    const int steps = std::clamp(depth, 0, 6);
    QSqlQuery query =
        library.query(QString("WITH RECURSIVE near(id, step) AS (SELECT ?, 0 "
                              "UNION SELECT target, step + 1 FROM graph_edges JOIN near ON source = near.id WHERE step < ?%1 "
                              "UNION SELECT source, step + 1 FROM graph_edges JOIN near ON target = near.id WHERE step < ?%1), "
                              "reached AS (SELECT DISTINCT id FROM near) "
                              "SELECT (SELECT COUNT(*) FROM reached), (SELECT COUNT(*) FROM graph_edges WHERE source IN reached "
                              "AND target IN reached%1)")
                          .arg(along),
                      {node, steps, steps});
    if (query.next()) {
        made.things = std::max(made.things, query.value(0).toLongLong());
        made.relations = std::max(made.relations, query.value(1).toLongLong());
    }
    return made;
}
