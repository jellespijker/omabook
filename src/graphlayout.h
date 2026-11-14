#pragma once

#include <QList>
#include <QPair>
#include <QPointF>
#include <QRectF>

// Where the nodes of a graph go on the page: a small simulation in which
// every node pushes every other away, an edge pulls its two ends together,
// and everything leans towards the middle, cooled a little with every step
// until nothing moves any more.
//
// It is stepped by whoever owns it, a few milliseconds at a time, so a graph
// of thousands never stalls the window; and nothing in it is left to chance,
// so the same graph comes out the same picture every time. The pushing apart
// is done over a quadtree (Barnes and Hut's way), which keeps a step of two
// thousand nodes well under a millisecond's worth of work per hundred.
class GraphLayout {
public:
    // How long an edge wants to be, which is also the scale of the picture.
    static constexpr double spacing = 70;

    // Starts over with `count` nodes and the edges among them, as pairs of
    // their indexes. A node that already has a place (`placed` is true for
    // it) starts from `known`, so a graph that only grew stays recognisable;
    // the others start beside the neighbours that have one, or on a spiral
    // around the middle, in an order that puts what is connected together.
    // `aspect` is how much wider than tall the picture should come out: a
    // window is wider than it is high, a pane beside the page the other way.
    //
    // Nodes of the `ring` stand around the others, outside them: what a
    // book's things connect to in other books. They are held by their edges
    // and kept out of the middle, and the others never feel them, so the
    // picture inside is the same with the ring as without it. A `fixed`
    // node stays exactly where it was placed.
    void start(int count, const QList<QPair<int, int>> &edges, const QList<QPointF> &known = {}, const QList<bool> &placed = {},
               double aspect = 1, const QList<bool> &ring = {}, const QList<bool> &fixed = {});
    // One step. False once the picture has settled.
    bool step();
    // Steps for at most that many milliseconds; false once settled.
    bool run(int milliseconds);
    // Steps until it has settled.
    void settle();

    bool settled() const { return m_heat <= 0; }
    int steps() const { return m_steps; }
    const QList<QPointF> &positions() const { return m_at; }
    // The box around every node.
    QRectF bounds() const;

private:
    struct Cell {
        double x = 0, y = 0, half = 0;      // the square it covers
        double massX = 0, massY = 0, mass = 0;
        int body = -1;                      // the one node in a leaf
        int child[4] = {-1, -1, -1, -1};
    };
    void seed(const QList<QPointF> &known, const QList<bool> &placed);
    void buildTree();
    void insert(int cell, int body, int depth);
    void push(int cell, int body, double &dx, double &dy) const;

    QList<QPointF> m_at;
    // The edges among the nodes inside, and those that hold the ring.
    QList<QPair<int, int>> m_edges, m_ties;
    QList<bool> m_ring, m_fixed;
    int m_inside = 0;
    bool m_insideDone = false;
    // The box around the nodes inside, which the ring stays out of.
    QRectF inside() const;
    void keepRingOut();
    QList<int> m_degree;
    QList<Cell> m_cells;
    double m_heat = 0;
    double m_aspect = 1;
    int m_steps = 0;
};
