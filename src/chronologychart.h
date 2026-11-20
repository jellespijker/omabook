#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtTypes>
#include <optional>

#include "calendars.h"
#include "chronicle.h"

// The chronology as a picture is laid out (#44): the chronicle (chronicle.h)
// of each part of the library, its axes stacked as bands, each band's lanes
// one under the other, and every event of a lane a mark at its place along
// the band. Pure: it says where things go, not how they are drawn (the
// window's ChronologyCanvas does that), so the same chronicle always lays
// out the same way.
//
// Along a band, every event has a place `at` from 0 (the band's earliest)
// to 1 (its latest), with a margin either side:
//
// - On a band with a calendar, by its time on that calendar: a moment at
//   the middle of the stretch its precision leaves open, a span from its
//   first moment to its last. An event of no time there goes between the
//   events with one by its rank, as far between them as its rank is.
// - On a band without a calendar, by its rank alone: the order and nothing
//   more is known.
//
// An event "during" another of the same lane is on a row under it; spans
// that overlap go on rows of their own, so that each can be seen.
namespace Chronology::Chart {

// A part of the library to show the chronicle of: a tenant, with its name
// when there are several (the top collection's), and what it is.
struct Part {
    QString name;
    Chronicle chronicle;
};

struct Band {
    int part = 0;
    QString calendar;                      // none when the band is by rank alone
    std::optional<Calendars::Scale> scale; // to read its minutes back as moments
    // What `at` 0 and 1 are on it: minutes of the calendar, or ranks.
    double first = 0, last = 1;
    QList<int> lanes;
    [[nodiscard]] bool dated() const { return scale.has_value(); }
    // A place along the band as a minute of its calendar, or a rank.
    [[nodiscard]] double value(double at) const { return first + at * (last - first); }
    [[nodiscard]] double at(double value) const { return last > first ? (value - first) / (last - first) : 0.5; }
};

struct LaneOf {
    int band = 0;
    GraphNode timeline; // id 0 for a book's own order (its bookId)
    int rows = 1;
};

struct Mark {
    qint64 event = 0;
    int lane = 0, row = 0;
    double from = 0, to = 0;               // along the band; the same for a moment
    bool span = false;                     // a span, rather than a moment
    bool openFrom = false, openTo = false; // a span with an end not given
    bool dated = false;                    // placed by its own time, rather than between others
    bool approximate = false;
    bool contradicted = false; // in a contradiction, said in its card
    qint64 within = 0;
};

struct Chart {
    QList<Band> bands;
    QList<LaneOf> lanes;
    QList<Mark> marks; // by lane, then along it
    QList<Item> unplaced;
    QList<Contradiction> contradictions; // every one, of every lane and part
    // Each event's marks, one for every lane it is on.
    QHash<qint64, QList<int>> marksOf;
    // The event of each, as the chronicle knows it, and its time.
    QHash<qint64, Item> items;
};

// The margin either side of a band, as a share of it.
inline constexpr double margin = 0.04;

[[nodiscard]] Chart chart(const QList<Part> &parts);

// The contradictions an event is in.
[[nodiscard]] QList<Contradiction> contradictionsOf(const Chart &chart, qint64 event);

} // namespace Chronology::Chart
