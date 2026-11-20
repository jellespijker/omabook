#pragma once

#include <QSet>
#include <QtTypes>
#include <optional>

#include "chronicle.h"
#include "chronologyorder.h"
#include "eventtime.h"

// A part of a chronicle (#45): the events of some books, of one timeline, or
// of a stretch of story time, as `omabook chronology` and its MCP tool ask
// for them. The orders are made of everything the reader knows first and
// only then narrowed, so that an event left out still places those around
// it: what is shown is in the order the whole chronicle has it.
namespace Chronology {

// Only the events met in these books, the timelines, and an event of no
// book (put in by hand) where a relation ties it to one of them.
[[nodiscard]] Facts ofBooks(Facts facts, const QSet<qint64> &books);

struct Narrowing {
    // Only this timeline's lane and the events on it; 0 for every timeline.
    qint64 timeline = 0;
    // Only what happens in this stretch: a span (from, to, either open) on
    // its calendar, or on each lane's own when it names none. An event placed
    // only by its relations stays when it lies among events that are in it.
    // Unplaced events have no time, so none is in it.
    std::optional<EventTime::Reckoning> span;

    [[nodiscard]] bool any() const { return timeline != 0 || span.has_value(); }
};

// The chronicle of `facts`, narrowed. The axes keep the lanes that are left,
// and of each contradiction only those about an event that is shown stay.
[[nodiscard]] Chronicle narrowed(Chronicle chronicle, const Facts &facts, const Narrowing &narrowing);

// The events in a chronicle, each counted once however many lanes it is on.
[[nodiscard]] QSet<qint64> eventsIn(const Chronicle &chronicle);

} // namespace Chronology
