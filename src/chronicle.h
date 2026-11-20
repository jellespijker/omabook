#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include "booksides.h"
#include "chronologyorder.h"
#include "graph.h"
#include "graphlayers.h"
#include "library.h"

// The chronology as a reader knows it (#43): of every timeline, its events
// in the order they happen (chronologyorder.h says how), and how the
// timelines line up with each other. What the window draws as lanes on an
// axis (#44), and what the command line and the MCP server tell (#45).
//
// - An event on several timelines is in the order of each.
// - An event that has a time or a relation but is on no timeline is in its
//   book's own order: a lane of no timeline (id 0) of the book it was first
//   met in.
// - An event of which neither a time nor a relation is known can't be put
//   in any order; it is listed apart, as unplaced.
// - Timelines line up on one axis when they share a calendar (or one tied
//   to it), an event, or a relation between their events: the axis is then
//   one order of all their events, by which the lanes are drawn side by side.
//
// It is read within one part of the library, a tenant: a scope of the graph
// and a side of the library (BookSides), so that a timeline shared by books
// in two top collections is two orders, one in each, never one across them.
namespace Chronology {

// Where a chronology is read: the graph's scope, and the side of the
// library (the top collections) whose books it is of. An empty side is the
// whole library, which is right only where it isn't split (tenants()).
struct Tenant {
    QString scope;
    BookSides::Side side;
};

// The parts a chronology of the library is read in: the whole of it, or one
// for each of its sides when they are kept apart.
[[nodiscard]] QList<Tenant> tenants(const Library &library, const QString &scope = {});

// Lanes that line up: their order together, by which each lane's events are
// put side by side. A lane alone is its own axis.
struct Axis {
    QList<int> lanes; // of Chronicle::lanes
    Lane order;       // all of their events in one order, on one calendar where they have one

    [[nodiscard]] QJsonObject toJson() const;
};

struct Chronicle {
    QList<Lane> lanes;                   // the timelines', by timeline, then the books' own, by book
    QList<Axis> axes;                    // every lane on exactly one
    QList<Item> unplaced;                // by where each was first met
    QList<Contradiction> contradictions; // of no one lane: ties between calendars

    [[nodiscard]] QJsonObject toJson() const;
};

// The chronicle of these facts.
[[nodiscard]] Chronicle chronicle(const Facts &facts);

// What a chronicle is made from, of the reader's own graph: the events and
// timelines known to whoever `known` says knows a place (as Chronology::of
// takes it), each event with its times, the places it was met and its
// relations, all cut there, and only the events of the tenant's side.
[[nodiscard]] Facts read(const Library &library, const GraphKnown &known, const Tenant &tenant);
// The same of a subscribed space's layer, which is cut at the reader's place already.
[[nodiscard]] Facts read(const GraphLayers::Layer &layer);
// Only the events met in a book on a side that meets `side`, and those of no book.
[[nodiscard]] Facts within(Facts facts, const BookSides &sides, const BookSides::Side &side);

} // namespace Chronology
