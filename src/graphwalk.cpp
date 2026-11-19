#include "graphwalk.h"

#include <QSet>

GraphView GraphWalk::within(const GraphView &view, qint64 centre, int depth) {
    QSet<qint64> reached{centre};
    QSet<qint64> frontier{centre};
    for (int walked = 0; walked < depth && !frontier.isEmpty(); ++walked) {
        QSet<qint64> next;
        for (const GraphEdge &edge : view.edges) {
            if (frontier.contains(edge.source) && !reached.contains(edge.target))
                next << edge.target;
            if (frontier.contains(edge.target) && !reached.contains(edge.source))
                next << edge.source;
        }
        reached += next;
        frontier = next;
    }
    GraphView near;
    for (const GraphNode &node : view.nodes)
        if (reached.contains(node.id))
            near.nodes << node;
    for (const GraphEdge &edge : view.edges)
        if (reached.contains(edge.source) && reached.contains(edge.target))
            near.edges << edge;
    return near;
}
