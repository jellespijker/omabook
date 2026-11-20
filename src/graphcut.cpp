#include "graphcut.h"

#include <QHash>
#include <QSet>
#include <QSqlQuery>
#include <QStringList>
#include <algorithm>
#include <utility>

#include "spaceworlds.h"
#include "worldlayers.h"

namespace {

[[nodiscard]] qint64 countOf(QSqlQuery query) { return query.next() ? query.value(0).toLongLong() : 0; }

} // namespace

// The relations among a scope's things, of the graph's tenant.
qint64 GraphCut::relationsIn(const Graph &graph, const QString &scope) {
    const std::optional<TenantFilter> &within = graph.within();
    return countOf(graph.library().query(QString("SELECT COUNT(*) FROM graph_edges JOIN graph_nodes AS source_node ON "
                                                 "source_node.id = source JOIN graph_nodes AS target_node ON target_node.id = "
                                                 "target WHERE source_node.scope = ? AND target_node.scope = ?%1")
                                             .arg(within ? " AND " + within->edges(u"graph_edges.id") : QString()),
                                         {scope, scope}));
}

// The relations that touch any of `ids`, either way along them, by their id.
// The walk around a thing asks the storage only here, a batch of things at a
// time, so the tenant it keeps to is said in this one place: a relation of
// another tenant is never gone along.
QHash<qint64, GraphCut::Ends> GraphCut::touching(const Graph &graph, const QList<qint64> &ids) {
    // Each id is bound twice: 800 stays under the fewest variables an SQLite
    // statement takes (999).
    constexpr qsizetype batch = 400;
    const std::optional<TenantFilter> &within = graph.within();
    const QString along = within ? " AND " + within->hasEdge(u"graph_edges.id") : QString();
    QHash<qint64, Ends> found;
    for (qsizetype from = 0; from < ids.size(); from += batch) {
        const QList<qint64> part = ids.mid(from, batch);
        const QString marks = QStringList(part.size(), QStringLiteral("?")).join(',');
        QVariantList values;
        values.reserve(part.size() * 2);
        for (int twice = 0; twice < 2; ++twice)
            for (const qint64 id : part)
                values << id;
        QSqlQuery query = graph.library().query(
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
std::pair<qint64, qint64> GraphCut::within(const Graph &graph, qint64 start, int steps) {
    QSet<qint64> reached{start};
    QHash<qint64, Ends> relations;
    QList<qint64> frontier{start};
    for (int step = 0; step <= steps && !frontier.isEmpty(); ++step) {
        const QHash<qint64, Ends> found = touching(graph, frontier);
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

GraphCut GraphCut::whole(const Graph &graph, const QString &scope, int limit) {
    // A world shared with this library is read as a plugin reads it, cut
    // where the reader is in its book, the same for the command line and the window.
    if (SpaceWorlds::isLayerName(scope))
        return GraphCut(WorldLayers::whole(graph, scope, limit));
    const QString in = scope.isNull() ? QString("") : scope;
    GraphCut made(graph.whole(in, limit));
    // Only a view that came to its limit can have left something out.
    if (made.view.nodes.size() < limit)
        return made;
    const std::optional<TenantFilter> &within = graph.within();
    made.things = std::max(
        made.things,
        countOf(graph.library().query(
            QString("SELECT COUNT(*) FROM graph_nodes WHERE scope = ?%1").arg(within ? " AND " + within->nodes(u"id") : QString()), {in})));
    if (made.cut())
        made.relations = std::max(made.relations, relationsIn(graph, in));
    return made;
}

GraphCut GraphCut::around(const Graph &graph, qint64 node, int depth, int limit) {
    GraphCut made(graph.around(node, depth, limit));
    made.by = By::Nearest;
    if (made.view.nodes.size() < limit)
        return made;
    // Everything within `depth` relations, both ways along each relation,
    // and the relations among what was reached.
    const auto [things, relations] = within(graph, node, std::clamp(depth, 0, 6));
    made.things = std::max(made.things, things);
    made.relations = std::max(made.relations, relations);
    return made;
}
