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
    // How the things shown were picked when not all of them could be.
    enum class By { Connected, Nearest };

    GraphView view;
    // In the whole the view was cut from: the view's own counts when
    // nothing was left out.
    qint64 things = 0, relations = 0;
    By by = By::Connected;

    GraphCut() = default;
    // A view of which nothing was left out.
    explicit GraphCut(GraphView whole);

    [[nodiscard]] bool cut() const noexcept { return view.nodes.size() < things; }
    // The view's JSON, with "total": {"nodes", "edges"} of the whole and,
    // when it was cut, "cut": "most connected" or "nearest".
    [[nodiscard]] QJsonObject toJson() const;

    // Graph::whole, with the scope's things and the relations among them; of
    // a layer, a world shared here (WorldLayers), what the reader knows of it.
    [[nodiscard]] static GraphCut whole(const Library &library, const QString &scope, int limit);
    // Graph::around, with what is within `depth` relations of the node.
    // Where the library's sides are apart (BookSides), the count goes along
    // no connection across books, so it never says more was left out than was.
    [[nodiscard]] static GraphCut around(const Library &library, qint64 node, int depth, int limit);

private:
    // The relations among a scope's things, as Graph answers them.
    [[nodiscard]] static qint64 relationsIn(const Library &library, const Graph &graph, const QString &scope);
    // A relation's two ends: its source and its target.
    using Ends = std::pair<qint64, qint64>;
    // How many things are within `steps` relations of `start`, and how many
    // relations there are among them, going along only relations `along` allows.
    [[nodiscard]] static std::pair<qint64, qint64> within(const Library &library, qint64 start, int steps, const QString &along);
    // The relations touching any of `ids`, by their id.
    [[nodiscard]] static QHash<qint64, Ends> touching(const Library &library, const QList<qint64> &ids, const QString &along);
};
