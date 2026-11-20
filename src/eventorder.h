#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QtTypes>
#include <limits>
#include <optional>

#include "calendars.h"
#include "chronologyorder.h"
#include "graph.h"

namespace Chronology {

// Arcs from each node, packed: those of node v are to[start[v]] up to to[start[v + 1]].
struct Adjacency {
    QList<int> start, to;
};

// How Orderer::order makes one order (chronologyorder.h says the rules):
// the events and the points in time their dates begin at are the nodes of a
// graph whose arcs say what comes no later than what (or strictly before
// it); circles with a strict arc in them are the contradictions, each
// found as the shortest circle through such an arc and broken by setting
// the earliest revealed relation on it aside, until none is left (so only a
// statement that is part of a contradiction is ever set aside); what is left in a circle
// happens at the same time; and the rest is laid out from the earliest,
// each event as near the time it is given or the neighbours it has as the
// arcs allow. It is linear in the events and relations, apart from the
// sorting, and one round more per contradiction.
class Arrangement {
public:
    Arrangement(const Orderer &orderer, QList<qint64> events);
    [[nodiscard]] Lane run(const GraphNode &timeline);

private:
    struct Arc {
        int from = 0, to = 0;
        bool strict = false;
        qint64 origin = 0; // the relation it comes of; 0 for one of the dates
    };

    void chooseScale();
    void checkTimes();
    void checkDates();
    void build();
    void during(const Orderer::Relation &relation);
    void resolve();
    // A depth-first walk through the graph: the arcs it takes back to a
    // node it is still in, each of which closes a circle, and the arc it
    // came to each node by (-1 for where it began).
    struct Walk {
        QList<int> back, cameBy;
    };
    [[nodiscard]] Walk walked(const Adjacency &arcs) const;
    // Whether the circle the walk closed with an arc back is still whole.
    [[nodiscard]] bool intact(int back, const Walk &walk) const;
    // The arcs of a circle the arc closes, within its component.
    [[nodiscard]] QList<int> circleThrough(int closing, const Adjacency &arcs, const QList<int> &component) const;
    [[nodiscard]] bool breakCircle(const QList<int> &arcs);
    // Breaks a circle in each component that has one; whether there was any.
    [[nodiscard]] bool breakAny(const Adjacency &arcs, const QList<int> &component);
    void setAside(qint64 edge);
    [[nodiscard]] QList<int> components() const;
    void place();

    // A class of nodes at the same time, and what places it.
    struct Class {
        std::optional<qint64> own; // where its time begins, or the point's
        int kind = 2;              // 0 a point, 1 an event with a date, 2 one without
        Revealed met{true, std::numeric_limits<qint64>::max(), 0, 0};
        qint64 first = std::numeric_limits<qint64>::max();
        QList<qint64> events;
        QList<int> next;
        int before = 0; // how many come before it
    };
    // The classes, each after all that come before it.
    [[nodiscard]] static QList<int> topological(const QList<Class> &classes);
    // The time each class is laid out by.
    [[nodiscard]] static QList<qint64> hints(const QList<Class> &classes);
    void layOut(QList<Class> &classes, const QList<qint64> &hint);

    [[nodiscard]] bool kept(const Orderer::Relation &relation) const { return !m_setAside.contains(relation.edge); }
    [[nodiscard]] int node(qint64 event) const { return m_index.value(event, -1); }
    [[nodiscard]] int nodes() const { return static_cast<int>(m_events.size() + m_points.size()); }
    [[nodiscard]] int pointAt(qint64 minute) const; // the first point at or after it; -1 if none
    [[nodiscard]] Source source(const Orderer::Relation &relation) const;
    [[nodiscard]] std::optional<Source> source(qint64 event) const;
    [[nodiscard]] static Source timeSource(qint64 event, const Dated &dated);
    void report(Contradiction::Kind kind, QList<qint64> events, QList<Source> sources, const Source &setAside);

    const Orderer *m_orderer; // outlives the arrangement
    QList<qint64> m_events;   // by number; node i is m_events[i]
    QHash<qint64, int> m_index;
    Calendars::Scale m_scale;
    QList<std::optional<Extent>> m_extents; // of each event
    QList<int> m_relations;                 // the orderer's of these events, by edge
    QSet<qint64> m_setAside;
    QList<qint64> m_points; // where dated events begin, earliest first; node m_events.size() + i
    QList<Arc> m_arcs;
    QList<bool> m_active;
    Lane m_lane;
};

} // namespace Chronology
