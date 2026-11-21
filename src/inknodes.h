#pragma once

#include <QHash>
#include <QSet>
#include <QString>

#include <span>
#include <vector>

#include "inklasso.h"
#include "inkmesh.h"
#include "inksheet.h"
#include "inkwriting.h"

class QSGGeometryNode;
class QSGNode;

// An ink surface's strokes in the scene graph (#78): a geometry node for each,
// kept from frame to frame, so that a frame redraws only the strokes that
// changed (the one being written) however many there are. All of them have
// the same material, so the renderer draws them in few batches.
class InkNodes {
public:
    // The strokes as they are shown, the lowest first: the sheet's, a
    // selection being dragged where it goes (#129), and those being written;
    // what changes from frame to frame, or was dragged and is let go, is dirty.
    static std::vector<const Ink::Stroke *> shown(const Ink::Sheet &sheet, const Ink::Writing &writing, Ink::Lasso &lasso,
                                                  QSet<QString> &dirty);

    // The root of the strokes' nodes: `old`, or a new one when the scene graph has none.
    QSGNode *root(QSGNode *old);

    struct Look {
        double scale = 1.0;   // item pixels to an ink unit
        double feather = 1.0; // a device pixel, in item pixels
    };

    // Brings the nodes in line with `strokes`, the lowest first: drops
    // those of strokes gone and orders them when `restructured`, makes those
    // of new strokes, and redraws the dirty ones (every one with `all`).
    void sync(std::span<const Ink::Stroke *const> strokes, const QSet<QString> &dirty, bool restructured, bool all, Look look);
    // What is drawn over the strokes (the lasso's marks, #129), in item pixels.
    void marks(const Ink::Mesh &mesh);

private:
    void reorder(std::span<const Ink::Stroke *const> strokes, QSet<QString> &drawn, Look look);
    QSGGeometryNode *nodeOf(const QString &id, bool &made);
    void redraw(QSGGeometryNode &node, const Ink::Stroke &stroke, Look look);
    static void fill(QSGGeometryNode &node, const Ink::Mesh &mesh);

    // All owned by the scene graph: the root holds the strokes' node, then the marks over them.
    QSGNode *m_top = nullptr;
    QSGNode *m_root = nullptr;
    QSGGeometryNode *m_marks = nullptr;
    QHash<QString, QSGGeometryNode *> m_nodes;
    Ink::Mesh m_mesh;
};
