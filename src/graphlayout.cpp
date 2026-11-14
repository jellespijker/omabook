#include "graphlayout.h"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <ranges>

namespace {

// The angle between one seed of a sunflower and the next: with it, any
// number of points fill a disc evenly and no two land on each other.
constexpr double golden = 2.39996322972865332;
// How far a cell may be, in its own widths, before it stands in for all
// the nodes inside it.
constexpr double farEnough = 0.9;
constexpr int deepest = 24;

[[nodiscard]] QPointF spiral(int rank, double step) {
    const double radius = step * std::sqrt(rank + 0.5);
    return {radius * std::cos(rank * golden), radius * std::sin(rank * golden)};
}

// What a list says of a node; nothing of one it doesn't reach, as a list
// left out says nothing of any.
[[nodiscard]] bool said(const QList<bool> &list, int node) { return node < list.size() && list.at(node); }

} // namespace

void GraphLayout::start(int count, const QList<QPair<int, int>> &edges, const QList<QPointF> &known, const QList<bool> &placed,
                        double aspect, const QList<bool> &ring, const QList<bool> &fixed) {
    m_aspect = std::clamp(aspect, 0.4, 2.5);
    m_edges.clear();
    m_ties.clear();
    m_degree = QList<int>(count, 0);
    m_ring = QList<bool>(count, false);
    m_fixed = QList<bool>(count, false);
    for (int i = 0; i < count; ++i) {
        m_ring[i] = said(ring, i);
        m_fixed[i] = said(fixed, i) && said(placed, i);
    }
    m_inside = int(std::ranges::count(m_ring, false));
    const auto isNode = [count](int node) { return node >= 0 && node < count; };
    for (const auto &edge : edges) {
        const auto [from, to] = edge;
        if (!isNode(from) || !isNode(to) || from == to)
            continue;
        // An edge to the ring holds the ring, and is nothing to the inside.
        if (m_ring.at(from) || m_ring.at(to)) {
            m_ties << edge;
            continue;
        }
        m_edges << edge;
        ++m_degree[from];
        ++m_degree[to];
    }
    m_at = QList<QPointF>(count);
    seed(known, placed);
    m_steps = 0;
    // A graph that kept most of its places only has to make room for what
    // is new; a new one has to find its whole shape.
    const auto kept = std::ranges::count(placed, true);
    const bool grown = count > 0 && kept * 2 >= count;
    const bool still = std::ranges::count(m_fixed, true) == count;
    // As hot as the inside needs, whatever stands around it: the inside is
    // to come out the same with a ring as without.
    m_heat = count < 2 || still ? 0 : grown ? spacing * 0.6 : spacing * std::clamp(std::sqrt(double(m_inside)) * 0.5, 1.5, 12.0);
    m_insideDone = false;
}

QRectF GraphLayout::inside() const {
    QRectF box;
    bool any = false;
    for (const auto &[at, ring] : std::views::zip(m_at, m_ring)) {
        if (ring)
            continue;
        const QRectF spot = QRectF(at, QSizeF(0, 0)).adjusted(-0.5, -0.5, 0.5, 0.5);
        box = any ? box.united(spot) : spot;
        any = true;
    }
    return box;
}

void GraphLayout::seed(const QList<QPointF> &known, const QList<bool> &placed) {
    const int count = int(m_at.size());
    QList<QList<int>> near(count);
    for (const auto &[from, to] : std::as_const(m_edges)) {
        near[from] << to;
        near[to] << from;
    }
    QList<bool> has(count, false);
    for (int i = 0; i < count; ++i)
        if (said(placed, i) && i < known.size()) {
            m_at[i] = known.at(i);
            has[i] = true;
        }
    // The rest in the order a walk from the best connected node meets them,
    // so neighbours get neighbouring seats on the spiral.
    QList<int> hubs(count);
    std::ranges::iota(hubs, 0);
    std::ranges::stable_sort(hubs, std::ranges::greater{}, [this](int node) { return m_degree.at(node); });
    QList<bool> walked(count, false);
    QList<int> order;
    order.reserve(count);
    for (const int hub : std::as_const(hubs)) {
        if (walked.at(hub))
            continue;
        walked[hub] = true;
        qsizetype next = order.size();
        order << hub;
        for (; next < order.size(); ++next)
            for (const int other : std::as_const(near[order.at(next)]))
                if (!walked.at(other)) {
                    walked[other] = true;
                    order << other;
                }
    }
    int rank = 0;
    for (const int node : std::as_const(order)) {
        if (has.at(node) || m_ring.at(node))
            continue;
        QPointF middle;
        int neighbours = 0;
        for (const int other : std::as_const(near[node]))
            if (has.at(other) && said(placed, other)) {
                middle += m_at.at(other);
                ++neighbours;
            }
        // Beside the neighbours that were here already, a little off them
        // so that two newcomers to the same node don't share a spot.
        if (neighbours > 0) {
            m_at[node] = middle / neighbours + spiral(node % 13, spacing * 0.1);
        } else {
            const QPointF seat = spiral(rank++, spacing * 0.75);
            m_at[node] = QPointF(seat.x() * std::sqrt(m_aspect), seat.y() / std::sqrt(m_aspect));
        }
        has[node] = true;
    }
    // The ring: each node out beyond the inside, on the side of what it is
    // tied to, so its edges start short and don't cross the picture.
    QList<QPointF> towards(count);
    QList<int> ties(count, 0);
    for (const auto &[from, to] : std::as_const(m_ties))
        for (const auto &[node, other] : {std::pair(from, to), std::pair(to, from)})
            if (m_ring.at(node) && !m_ring.at(other)) {
                towards[node] += m_at.at(other);
                ++ties[node];
            }
    const QRectF box = inside();
    const double out = std::max(box.width(), box.height()) / 2 + spacing * 2;
    for (int node = 0; node < count; ++node) {
        if (!m_ring.at(node) || has.at(node))
            continue;
        QPointF way = ties.at(node) > 0 ? towards.at(node) / ties.at(node) - box.center() : QPointF();
        const double length = std::hypot(way.x(), way.y());
        way = length > 1 ? way / length : QPointF(std::cos(node * golden), std::sin(node * golden));
        m_at[node] = box.center() + way * out + spiral(node % 13, spacing * 0.1);
    }
}

void GraphLayout::insert(int cell, int body, int depth) {
    const QPointF at = m_at.at(body);
    while (true) {
        Cell &here = m_cells[cell];
        here.massX += at.x();
        here.massY += at.y();
        here.mass += 1;
        // An empty leaf takes the node; so does a cell too small to split,
        // which only happens to nodes on the very same spot.
        if ((here.mass == 1 && here.body < 0) || depth >= deepest) {
            if (here.body < 0)
                here.body = body;
            return;
        }
        if (here.body >= 0) {
            // A leaf that gets a second node hands its first down a level.
            const int first = here.body;
            here.body = -1;
            const QPointF was = m_at.at(first);
            const int quarter = here.quarterOf(was);
            Cell child = here.quarter(quarter);
            child.massX = was.x();
            child.massY = was.y();
            child.mass = 1;
            child.body = first;
            m_cells << child;
            m_cells[cell].child[quarter] = int(m_cells.size()) - 1;
        }
        const Cell &parent = m_cells.at(cell);
        const int quarter = parent.quarterOf(at);
        if (parent.child[quarter] < 0) {
            m_cells << parent.quarter(quarter);
            m_cells[cell].child[quarter] = int(m_cells.size()) - 1;
        }
        cell = m_cells.at(cell).child[quarter];
        ++depth;
    }
}

void GraphLayout::buildTree() {
    const QRectF box = inside();
    m_cells.clear();
    m_cells.reserve(m_at.size() * 2 + 8);
    m_cells << Cell{.x = box.center().x(), .y = box.center().y(), .half = std::max({box.width(), box.height(), 1.0}) / 2 + 1};
    // Only the nodes inside push: the ring is pushed by them, not they by it.
    for (int body = 0; body < m_at.size(); ++body)
        if (!m_ring.at(body))
            insert(0, body, 0);
}

// What everything in a cell does to one node: it pushes it away, as the one
// mass in its middle when it is far off, cell by cell when it is near.
void GraphLayout::push(int cell, int body, double &dx, double &dy) const {
    const Cell &here = m_cells.at(cell);
    if (here.mass == 0 || here.body == body)
        return;
    const QPointF at = m_at.at(body);
    double x = at.x() - here.massX / here.mass, y = at.y() - here.massY / here.mass;
    double distance = std::hypot(x, y);
    const bool leaf = here.body >= 0;
    if (leaf || here.half * 2 < farEnough * distance) {
        if (distance < 0.01) {
            // On the same spot there is no way to point, so each takes its own.
            x = std::cos(body * golden);
            y = std::sin(body * golden);
            distance = 0.01;
        }
        // It falls off with the square of the distance, so what is far
        // away matters little and a large graph stays as dense as a small one.
        const double force = spacing * spacing * spacing * here.mass / (distance * distance);
        dx += x / distance * force;
        dy += y / distance * force;
        return;
    }
    for (const int child : here.child)
        if (child >= 0)
            push(child, body, dx, dy);
}

bool GraphLayout::step() {
    if (settled())
        return false;
    const int count = int(m_at.size());
    buildTree();
    QList<double> dx(count, 0), dy(count, 0);
    // The lean towards the middle is what holds the picture together against
    // all that pushing, loose pieces included. It is as strong as it takes
    // for every node to have about a spacing's square to itself, however
    // many there are; a well connected node leans a little harder, which
    // puts the hubs in the centre.
    const double lean = std::min(0.8, 1.9 / std::sqrt(double(std::max(1, m_inside))));
    // Leaning less sideways than up and down, or the other way round, is
    // what makes the picture as wide or as tall as its view.
    const double sideways = 1 / m_aspect, upright = m_aspect;
    for (int i = 0; i < count; ++i) {
        push(0, i, dx[i], dy[i]);
        if (m_ring.at(i))
            continue;
        const double pull = lean * (1 + m_degree.at(i) * 0.08);
        dx[i] -= m_at.at(i).x() * pull * sideways;
        dy[i] -= m_at.at(i).y() * pull * upright;
    }
    // The ring: its nodes keep off each other, and each is drawn to what
    // it is tied to. There are few of them, so each is weighed against each.
    const auto ring = std::views::iota(0, count) | std::views::filter([this](int node) { return m_ring.at(node); })
                      | std::ranges::to<QList<int>>();
    for (qsizetype a = 0; a < ring.size(); ++a)
        for (qsizetype b = a + 1; b < ring.size(); ++b) {
            QPointF apart = m_at.at(ring.at(a)) - m_at.at(ring.at(b));
            double distance = std::hypot(apart.x(), apart.y());
            if (distance < 0.01) {
                apart = QPointF(std::cos(a * golden), std::sin(a * golden));
                distance = 0.01;
            }
            const double force = 2 * spacing * spacing * spacing / (distance * distance);
            dx[ring.at(a)] += apart.x() / distance * force;
            dy[ring.at(a)] += apart.y() / distance * force;
            dx[ring.at(b)] -= apart.x() / distance * force;
            dy[ring.at(b)] -= apart.y() / distance * force;
        }
    for (const auto &[from, to] : std::as_const(m_ties)) {
        const QPointF between = m_at.at(to) - m_at.at(from);
        const double distance = std::max(0.01, std::hypot(between.x(), between.y()));
        const double force = distance * distance / spacing / 2;
        const double x = between.x() / distance * force, y = between.y() / distance * force;
        if (m_ring.at(from)) {
            dx[from] += x;
            dy[from] += y;
        }
        if (m_ring.at(to)) {
            dx[to] -= x;
            dy[to] -= y;
        }
    }
    for (const auto &[from, to] : std::as_const(m_edges)) {
        const QPointF between = m_at.at(to) - m_at.at(from);
        const double distance = std::max(0.01, std::hypot(between.x(), between.y()));
        // A node with many edges would be crushed by all of them pulling
        // at once, so each pulls less the more there are.
        const double share = 1 + std::log2(1.0 + std::min(m_degree.at(from), m_degree.at(to)));
        const double force = distance * distance / spacing / share;
        const double x = between.x() / distance * force, y = between.y() / distance * force;
        dx[from] += x;
        dy[from] += y;
        dx[to] -= x;
        dy[to] -= y;
    }
    double furthest = 0;
    double furthestInside = 0;
    for (int i = 0; i < count; ++i) {
        const double wanted = std::hypot(dx.at(i), dy.at(i));
        if (wanted < 1e-9 || m_fixed.at(i) || (m_insideDone && !m_ring.at(i)))
            continue;
        // No node moves further in a step than the heat allows.
        const double moved = std::min(wanted, m_heat);
        m_at[i] += QPointF(dx.at(i) / wanted * moved, dy.at(i) / wanted * moved);
        double &reach = m_ring.at(i) ? furthest : furthestInside;
        reach = std::max(reach, moved);
    }
    keepRingOut();
    ++m_steps;
    m_heat *= 0.972;
    // The inside comes to rest by itself, as it would with no ring around
    // it; the ring goes on until it has too.
    if (furthestInside < 0.05)
        m_insideDone = true;
    if (m_heat < 0.4 || (m_insideDone && furthest < 0.05) || m_steps >= 400)
        m_heat = 0;
    return !settled();
}

// The ring stays out of the box around the inside, and a little off it: a
// node of it that is within is put on the edge of that margin, out along
// the line from the middle through it. This is done after the inside has
// moved, against where the inside is now, so that it holds when the layout
// stops, on whichever step that is.
void GraphLayout::keepRingOut() {
    if (m_inside == 0 || m_inside == m_at.size())
        return;
    const QRectF box = inside();
    const double across = box.width() / 2 + spacing * 1.1, down = box.height() / 2 + spacing * 1.1;
    for (int i = 0; i < m_at.size(); ++i) {
        if (!m_ring.at(i) || m_fixed.at(i))
            continue;
        QPointF from = m_at.at(i) - box.center();
        if (std::abs(from.x()) >= across || std::abs(from.y()) >= down)
            continue;
        // In the very middle there is no way out to follow, so each takes its own.
        if (std::hypot(from.x(), from.y()) < 1e-6)
            from = QPointF(std::cos(i * golden), std::sin(i * golden));
        const double out = std::min(std::abs(from.x()) > 1e-12 ? across / std::abs(from.x()) : 1e300,
                                    std::abs(from.y()) > 1e-12 ? down / std::abs(from.y()) : 1e300);
        m_at[i] = box.center() + from * out;
    }
}

bool GraphLayout::run(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (step())
        if (clock.elapsed() >= milliseconds)
            return true;
    return false;
}

void GraphLayout::settle() {
    while (step()) {
    }
}

QRectF GraphLayout::bounds() const {
    if (m_at.isEmpty())
        return {};
    const auto across = m_at | std::views::transform(&QPointF::x);
    const auto down = m_at | std::views::transform(&QPointF::y);
    const double left = std::ranges::min(across), top = std::ranges::min(down);
    return {left, top, std::ranges::max(across) - left, std::ranges::max(down) - top};
}
