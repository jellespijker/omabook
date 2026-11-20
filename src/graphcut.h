#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTypes>
#include <utility>

#include "graph.h"
#include "library.h"

// A view of the graph cut at a limit, and how much it was cut from: a whole
// scope, its most connected things first, or what is around a thing, the
// nearest first. Neither loads more than it shows. How much there is besides
// is counted in the storage, and only when the view came to its limit, so
// that a window, a command or an agent can say "2,000 of 6,965 things"
// rather than pass a part off as the whole.
struct GraphCut {
    // How the things shown were picked when not all of them could be: the
    // most connected, the nearest, or the connections between books whose
    // ends are the most connected (across).
    enum class By { Connected, Nearest, Across };

    GraphView view;
    // In the whole the view was cut from: the view's own counts when
    // nothing was left out.
    qint64 things = 0, relations = 0;
    By by = By::Connected;

    GraphCut() = default;
    // A view of which nothing was left out.
    explicit GraphCut(GraphView whole);

    // Of the connections between books, `relations` counts the connections
    // there are and `connections` those shown, which the books they join and
    // the ties to them (not edges the storage holds) come on top of.
    qint64 connections = 0;

    [[nodiscard]] bool cut() const noexcept { return by == By::Across ? connections < relations : view.nodes.size() < things; }
    // The view's JSON, with "total": {"nodes", "edges"} of the whole and,
    // when it was cut, "cut": "most connected", "nearest" or "across".
    [[nodiscard]] QJsonObject toJson() const;
    // How the things shown were picked, as toJson says it: "most connected",
    // "nearest" or "across".
    [[nodiscard]] QString how() const;

    // Graph::whole, with the scope's things and the relations among them; of
    // a layer, a world shared here (WorldLayers), what the reader knows of it.
    // Read as the graph's tenant sees it, and counted so too.
    [[nodiscard]] static GraphCut whole(const Graph &graph, const QString &scope, int limit);
    // Graph::around, with what is within `depth` relations of the node, of
    // the graph's tenant.
    [[nodiscard]] static GraphCut around(const Graph &graph, qint64 node, int depth, int limit);
    // The connections between books (Graph::acrossBooks), at most `limit`,
    // each thing at their ends with the books of the tenant it is in, and
    // how many connections, and things they join, the tenant has in all.
    [[nodiscard]] static GraphCut across(const Graph &graph, int limit);

private:
    // The relations among a scope's things, as Graph answers them.
    [[nodiscard]] static qint64 relationsIn(const Graph &graph, const QString &scope);
    // A relation's two ends: its source and its target.
    using Ends = std::pair<qint64, qint64>;
    // How many things are within `steps` relations of `start`, and how many
    // relations there are among them.
    [[nodiscard]] static std::pair<qint64, qint64> within(const Graph &graph, qint64 start, int steps);
    // The relations touching any of `ids`, by their id, of the graph's tenant (#100).
    [[nodiscard]] static QHash<qint64, Ends> touching(const Graph &graph, const QList<qint64> &ids);
    // Adds to an across view the books its things are in, each tied to them.
    static void withBooks(const Graph &graph, GraphView &view);
};
