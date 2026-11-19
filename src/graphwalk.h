#pragma once

#include <QtTypes>

#include "graph.h"

// Walking a view of the graph already read, without the database: what a
// window or a command cut out of a book's graph, around one thing.
namespace GraphWalk {

// The nodes within `depth` edges of one, in the view's order, and the edges
// among them.
[[nodiscard]] GraphView within(const GraphView &view, qint64 centre, int depth);

} // namespace GraphWalk
