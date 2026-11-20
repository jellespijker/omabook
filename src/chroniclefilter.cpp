#include "chroniclefilter.h"

#include <algorithm>
#include <ranges>

#include "calendars.h"
#include "chronology.h"

namespace {

using Chronology::Contradiction;
using Chronology::Item;
using Chronology::Lane;

[[nodiscard]] bool about(const Contradiction &contradiction, const QSet<qint64> &shown) {
    return std::ranges::any_of(contradiction.events, [&shown](qint64 event) { return shown.contains(event); });
}

[[nodiscard]] QSet<qint64> eventsOf(const Lane &lane) {
    QSet<qint64> events;
    for (const Item &item : lane.items)
        events.insert(item.event.id);
    return events;
}

// A lane's events in the stretch, which is put on the lane's calendar
// through the ties between calendars; nothing of a lane it doesn't reach.
[[nodiscard]] Lane inSpan(Lane lane, const Chronology::Calendars &calendars, const EventTime::Reckoning &span) {
    EventTime window;
    window.on = span;
    if (window.on.calendar.isEmpty())
        window.on.calendar = lane.calendar;
    const std::optional<Chronology::Extent> stretch =
        lane.calendar.isEmpty() ? std::nullopt : calendars.scale(lane.calendar).extent(window);
    const auto in = [&stretch](const Item &item) { return item.extent && item.extent->meets(*stretch); };
    auto ranks = lane.items | std::views::filter(in) | std::views::transform(&Item::rank);
    if (!stretch || std::ranges::empty(ranks)) {
        lane.items.clear();
        return lane;
    }
    const int first = std::ranges::min(ranks);
    const int last = std::ranges::max(ranks);
    lane.items.removeIf([&](const Item &item) { return item.extent ? !in(item) : item.rank < first || item.rank > last; });
    return lane;
}

// The lane narrowed, with only the contradictions about what is left of it.
[[nodiscard]] Lane narrowedLane(const Lane &lane, const Chronology::Calendars &calendars, const Chronology::Narrowing &narrowing) {
    Lane left = narrowing.span ? inSpan(lane, calendars, *narrowing.span) : lane;
    const QSet<qint64> shown = eventsOf(left);
    left.contradictions.removeIf([&shown](const Contradiction &contradiction) { return !about(contradiction, shown); });
    return left;
}

} // namespace

Chronology::Facts Chronology::ofBooks(Facts facts, const QSet<qint64> &books) {
    QSet<qint64> kept;
    QSet<qint64> ofNoBook;
    for (const GraphNode &event : std::as_const(facts.events)) {
        const QList<GraphPlace> places = facts.met.value(event.id);
        if (places.isEmpty())
            ofNoBook.insert(event.id);
        else if (std::ranges::any_of(places, [&books](const GraphPlace &place) { return books.contains(place.bookId); }))
            kept.insert(event.id);
    }
    QSet<qint64> tied;
    for (const GraphEdge &edge : std::as_const(facts.edges))
        if (orderRelations().contains(edge.kind)) {
            if (kept.contains(edge.source) && ofNoBook.contains(edge.target))
                tied.insert(edge.target);
            if (kept.contains(edge.target) && ofNoBook.contains(edge.source))
                tied.insert(edge.source);
        }
    kept += tied;
    facts.events.removeIf([&kept](const GraphNode &event) { return !kept.contains(event.id); });
    QSet<qint64> things = kept;
    for (const GraphNode &timeline : std::as_const(facts.timelines))
        things.insert(timeline.id);
    facts.edges.removeIf([&things](const GraphEdge &edge) { return !things.contains(edge.source) || !things.contains(edge.target); });
    // The times and places of the events left out stay in their hashes, unread: only the events' are looked up.
    return facts;
}

Chronology::Chronicle Chronology::narrowed(Chronicle chronicle, const Facts &facts, const Narrowing &narrowing) {
    if (!narrowing.any())
        return chronicle;
    const Orderer orderer(facts);
    QList<qsizetype> kept; // where each lane left was
    QList<Lane> lanes;
    for (const auto [at, lane] : std::views::enumerate(std::as_const(chronicle.lanes))) {
        if (narrowing.timeline != 0 && lane.timeline.id != narrowing.timeline)
            continue;
        Lane left = narrowedLane(lane, orderer.calendars(), narrowing);
        if (left.items.isEmpty())
            continue;
        kept.append(at);
        lanes.append(std::move(left));
    }
    QSet<qint64> onTimeline;
    for (const GraphEdge &edge : facts.edges)
        if (edge.kind == onRelation() && edge.target == narrowing.timeline)
            onTimeline.insert(edge.source);
    chronicle.unplaced.removeIf(
        [&](const Item &item) { return narrowing.span || (narrowing.timeline != 0 && !onTimeline.contains(item.event.id)); });
    QList<Axis> axes;
    for (Axis axis : std::as_const(chronicle.axes)) {
        QList<int> members;
        QSet<qint64> shown;
        for (const int old : std::as_const(axis.lanes))
            if (const qsizetype now = kept.indexOf(old); now >= 0) {
                members.append(static_cast<int>(now));
                shown += eventsOf(lanes.at(now));
            }
        if (members.isEmpty())
            continue;
        axis.lanes = members;
        axis.order.items.removeIf([&shown](const Item &item) { return !shown.contains(item.event.id); });
        axis.order.contradictions.removeIf([&shown](const Contradiction &contradiction) { return !about(contradiction, shown); });
        axes.append(std::move(axis));
    }
    chronicle.lanes = std::move(lanes);
    chronicle.axes = std::move(axes);
    const QSet<qint64> shown = eventsIn(chronicle);
    chronicle.contradictions.removeIf([&shown](const Contradiction &contradiction) { return !about(contradiction, shown); });
    return chronicle;
}

QSet<qint64> Chronology::eventsIn(const Chronicle &chronicle) {
    QSet<qint64> events;
    for (const Lane &lane : chronicle.lanes)
        events += eventsOf(lane);
    for (const Item &item : chronicle.unplaced)
        events.insert(item.event.id);
    return events;
}
