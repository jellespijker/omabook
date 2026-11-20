#include "graphcut.h"

#include <QHash>
#include <QSet>
#include <QSqlQuery>
#include <QStringList>
#include <algorithm>
#include <limits>
#include <utility>

#include "spaceworlds.h"
#include "worldlayers.h"

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

// The relations that touch any of `ids`, either way along them, by their id.
// The walk around a thing asks the storage only here, a batch of things at a
// time, so whatever narrows the relations it may go along (`along`) is said
// in this one place.
QHash<qint64, GraphCut::Ends> GraphCut::touching(const Library &library, const QList<qint64> &ids, const QString &along) {
    // Each id is bound twice: 800 stays under the fewest variables an SQLite
    // statement takes (999).
    constexpr qsizetype batch = 400;
    QHash<qint64, Ends> found;
    for (qsizetype from = 0; from < ids.size(); from += batch) {
        const QList<qint64> part = ids.mid(from, batch);
        const QString marks = QStringList(part.size(), QStringLiteral("?")).join(',');
        QVariantList values;
        values.reserve(part.size() * 2);
        for (int twice = 0; twice < 2; ++twice)
            for (const qint64 id : part)
                values << id;
        QSqlQuery query = library.query(
            QString("SELECT id, source, target FROM graph_edges WHERE (source IN (%1) OR target IN (%1))%2").arg(marks, along), values);
        while (query.next())
            found.insert(query.value(0).toLongLong(), Ends{query.value(1).toLongLong(), query.value(2).toLongLong()});
    }
    return found;
}

// How many things are within `steps` relations of `start`, and how many
// relations there are among them. Walked level by level: each thing's
// relations are asked for once, when the walk first reaches it, so a hub
// reached at every step costs no more than one reached once. Every relation
// among what was reached touches a thing of some level, the last one too, so
// they are all among those asked for.
std::pair<qint64, qint64> GraphCut::within(const Library &library, qint64 start, int steps, const QString &along) {
    QSet<qint64> reached{start};
    QHash<qint64, Ends> relations;
    QList<qint64> frontier{start};
    for (int step = 0; step <= steps && !frontier.isEmpty(); ++step) {
        const QHash<qint64, Ends> found = touching(library, frontier, along);
        frontier.clear();
        for (const auto &[id, relation] : found.asKeyValueRange()) {
            relations.insert(id, relation);
            if (step == steps)
                continue;
            for (const qint64 end : {relation.first, relation.second})
                if (!reached.contains(end)) {
                    reached.insert(end);
                    frontier << end;
                }
        }
    }
    const qint64 among = std::count_if(relations.cbegin(), relations.cend(), [&](const Ends &relation) {
        return reached.contains(relation.first) && reached.contains(relation.second);
    });
    return {reached.size(), among};
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
    // A world shared with this library is read as a plugin reads it, cut
    // where the reader is in its book, the same for the command line and the window.
    if (SpaceWorlds::isLayerName(scope))
        return GraphCut(WorldLayers::whole(library, scope, limit));
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
    // Everything within `depth` relations, both ways along each relation,
    // and the relations among what was reached.
    const QString along = graph.sides().apart() ? " AND kind NOT LIKE 'across: %'" : "";
    const auto [things, relations] = within(library, node, std::clamp(depth, 0, 6), along);
    made.things = std::max(made.things, things);
    made.relations = std::max(made.relations, relations);
    return made;
}
