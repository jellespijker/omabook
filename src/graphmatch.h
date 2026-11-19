#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>

class Graph;

// When a thing of another computer's knowledge graph is a thing of this one:
// of the same kind, and going by the same name or one of its other names,
// its own name first, then the one with most names in common, and never two
// things here for one there.
namespace GraphMatch {

// For each thing of `nodes` ({"id", "kind", "name", "aliases"}, as a graph
// bundle has them), the thing of the general graph here it is, or 0: how
// GraphSync::take matches what an older Omabook sends, and how the log
// links, once, what two computers know by different names (synctakegraph.h).
[[nodiscard]] QList<qint64> matchNodes(const Graph &graph, const QJsonArray &nodes);

} // namespace GraphMatch
