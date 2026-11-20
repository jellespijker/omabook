#include "chronologychart.h"

#include <QSet>
#include <algorithm>
#include <limits>
#include <numeric>
#include <ranges>
#include <utility>

using namespace Chronology;
using Chart::Band;
using Chart::Mark;

namespace {

// Where an event is along an axis, before it is put between 0 and 1.
struct Placing {
    double from = 0, to = 0;
    bool span = false, openFrom = false, openTo = false, dated = false;
};

// A day, for a band of one dated event: its stretch has to be some length.
constexpr double aDay = 24.0 * 60;
// The most rows a lane takes for spans that overlap: past that they overlap.
constexpr int mostRows = 6;
// How deep "during" is followed, so that a circle of them still ends.
constexpr int deepest = 16;

[[nodiscard]] Placing ofExtent(const Extent &extent, const Item &item) {
    const bool span = item.time && item.time->time.span();
    const double from = static_cast<double>(extent.from.value_or(extent.to.value_or(0)));
    const double to = static_cast<double>(extent.to.value_or(extent.from.value_or(0)));
    if (!span) {
        // A moment is in the middle of the stretch its precision leaves open.
        const double middle = (from + to) / 2;
        return {.from = middle, .to = middle, .span = false, .openFrom = false, .openTo = false, .dated = true};
    }
    return {.from = from, .to = to, .span = true, .openFrom = !extent.from, .openTo = !extent.to, .dated = true};
}

// Every event of an axis where it goes along it: by time where it has one on
// the axis's calendar, by rank between those that do otherwise.
[[nodiscard]] QHash<qint64, Placing> placings(const Lane &order, bool dated) {
    QHash<qint64, Placing> placed;
    QList<std::pair<int, double>> anchors; // the rank and the middle of each dated event
    for (const Item &item : order.items)
        if (dated && item.extent && (item.extent->from || item.extent->to)) {
            const Placing placing = ofExtent(*item.extent, item);
            placed.insert(item.event.id, placing);
            anchors.append({item.rank, (placing.from + placing.to) / 2});
        }
    if (!dated) {
        for (const Item &item : order.items)
            placed.insert(item.event.id, {.from = static_cast<double>(item.rank), .to = static_cast<double>(item.rank)});
        return placed;
    }
    std::ranges::stable_sort(anchors, {}, &std::pair<int, double>::first);
    const double ranks = anchors.size() > 1 ? anchors.last().first - anchors.first().first : 0;
    const double gap = ranks > 0 ? std::max((anchors.last().second - anchors.first().second) / ranks, aDay) : aDay;
    for (const Item &item : order.items) {
        if (placed.contains(item.event.id))
            continue;
        const auto next = std::ranges::lower_bound(anchors, item.rank, {}, &std::pair<int, double>::first);
        double at = 0;
        if (next == anchors.cbegin()) {
            at = next->second - (next->first - item.rank) * gap;
        } else if (next == anchors.cend()) {
            at = std::prev(next)->second + (item.rank - std::prev(next)->first) * gap;
        } else {
            const auto before = std::prev(next);
            const double between = next->first - before->first;
            at = before->second + (between > 0 ? (next->second - before->second) * (item.rank - before->first) / between : 0);
        }
        placed.insert(item.event.id, {.from = at, .to = at});
    }
    return placed;
}

// The band's ends: what 0 and 1 along it are, with a margin either side.
void frame(Band &band, const QHash<qint64, Placing> &placed) {
    double low = 0;
    double high = 0;
    bool any = false;
    for (const Placing &placing : placed) {
        low = any ? std::min(low, placing.from) : placing.from;
        high = any ? std::max(high, placing.to) : placing.to;
        any = true;
    }
    // A band of one moment is a year wide, or a few ranks.
    double span = band.dated() ? 365 * aDay : 1;
    if (high > low)
        span = high - low;
    const double middle = (low + high) / 2;
    if (high <= low) {
        low = middle - span / 2;
        high = middle + span / 2;
    }
    const double whole = (high - low) / (1 - 2 * Chart::margin);
    band.first = low - Chart::margin * whole;
    band.last = band.first + whole;
}

// How deep in "during" each event of a lane is: 0 for one during none of them.
[[nodiscard]] QHash<qint64, int> depths(const Lane &lane) {
    QHash<qint64, qint64> within;
    for (const Item &item : lane.items)
        if (item.within != 0)
            within.insert(item.event.id, item.within);
    QHash<qint64, int> depth;
    for (const Item &item : lane.items) {
        int deep = 0;
        for (qint64 at = item.event.id; within.contains(at) && deep < deepest; at = within.value(at))
            ++deep;
        depth.insert(item.event.id, deep);
    }
    return depth;
}

// Rows for a lane's marks: under what each is during, and apart from the
// spans it would overlap.
int arrange(QList<Mark> &marks, const QHash<qint64, int> &depth) {
    QList<int> order(marks.size());
    std::ranges::iota(order, 0);
    std::ranges::stable_sort(order, {}, [&](int at) { return std::pair{marks.at(at).from, depth.value(marks.at(at).event)}; });
    QHash<qint64, int> rowOf;
    QList<double> end; // of each row, the furthest it is taken
    for (const int at : order) {
        Mark &mark = marks[at];
        int row = mark.within != 0 && rowOf.contains(mark.within) ? rowOf.value(mark.within) + 1 : 0;
        for (; row < mostRows - 1 && row < end.size(); ++row)
            if (mark.span ? end.at(row) < mark.from : end.at(row) <= mark.from)
                break;
        row = std::min(row, mostRows - 1);
        while (end.size() <= row)
            end.append(-std::numeric_limits<double>::infinity());
        end[row] = std::max(end.at(row), mark.to);
        mark.row = row;
        rowOf.insert(mark.event, row);
    }
    return std::max(1, static_cast<int>(end.size()));
}

[[nodiscard]] QSet<qint64> contradicted(const QList<Contradiction> &contradictions) {
    QSet<qint64> events;
    for (const Contradiction &contradiction : contradictions)
        for (const qint64 event : contradiction.events)
            events.insert(event);
    return events;
}

[[nodiscard]] QList<Contradiction> everyContradiction(const Chronicle &chronicle) {
    QList<Contradiction> all = chronicle.contradictions;
    for (const Lane &lane : chronicle.lanes)
        all += lane.contradictions;
    for (const Axis &axis : chronicle.axes)
        if (axis.lanes.size() > 1)
            all += axis.order.contradictions;
    // A lane and the axis it is on can find the same one: it is said once.
    QSet<std::pair<qint64, std::pair<qint64, qint64>>> seen;
    all.removeIf([&seen](const Contradiction &contradiction) {
        const std::pair key{static_cast<qint64>(contradiction.kind), std::pair{contradiction.setAside.edge, contradiction.setAside.fact}};
        if (seen.contains(key))
            return true;
        seen.insert(key);
        return false;
    });
    return all;
}

[[nodiscard]] Band bandOf(int part, const Axis &axis) {
    Band band;
    band.part = part;
    const bool dated = !axis.order.calendar.isEmpty() && std::ranges::any_of(axis.order.items, [](const Item &item) {
        return item.extent && (item.extent->from || item.extent->to);
    });
    if (dated) {
        band.calendar = axis.order.calendar;
        QList<Dated> times;
        for (const Item &item : axis.order.items)
            if (item.time)
                times.append(*item.time);
        band.scale = Calendars(times).scale(band.calendar);
    }
    return band;
}

} // namespace

Chart::Chart Chart::chart(const QList<Part> &parts) {
    Chart drawn;
    for (const auto [part, of] : std::views::enumerate(parts)) {
        const Chronicle &chronicle = of.chronicle;
        const QList<Contradiction> contradictions = everyContradiction(chronicle);
        const QSet<qint64> troubled = contradicted(contradictions);
        drawn.contradictions += contradictions;
        drawn.unplaced += chronicle.unplaced;
        for (const Axis &axis : chronicle.axes) {
            Band band = bandOf(static_cast<int>(part), axis);
            const QHash<qint64, Placing> placed = placings(axis.order, band.dated());
            frame(band, placed);
            for (const int index : axis.lanes) {
                const Lane &lane = chronicle.lanes.at(index);
                const int laneAt = static_cast<int>(drawn.lanes.size());
                band.lanes.append(laneAt);
                QList<Mark> marks;
                for (const Item &item : lane.items) {
                    const Placing placing = placed.value(item.event.id);
                    marks.append({.event = item.event.id,
                                  .lane = laneAt,
                                  .row = 0,
                                  .from = band.at(placing.from),
                                  .to = band.at(placing.span && placing.openTo ? band.last : placing.to),
                                  .span = placing.span,
                                  .openFrom = placing.openFrom,
                                  .openTo = placing.openTo,
                                  .dated = placing.dated,
                                  .approximate = item.time && item.time->time.approximate,
                                  .contradicted = troubled.contains(item.event.id),
                                  .within = item.within});
                    if (placing.span && placing.openFrom)
                        marks.last().from = 0;
                    if (!drawn.items.contains(item.event.id) || (item.time && !drawn.items.value(item.event.id).time))
                        drawn.items.insert(item.event.id, item);
                }
                const int rows = arrange(marks, depths(lane));
                for (const Mark &mark : std::as_const(marks)) {
                    drawn.marksOf[mark.event].append(static_cast<int>(drawn.marks.size()));
                    drawn.marks.append(mark);
                }
                drawn.lanes.append({.band = static_cast<int>(drawn.bands.size()), .timeline = lane.timeline, .rows = rows});
            }
            drawn.bands.append(std::move(band));
        }
    }
    for (const Item &item : std::as_const(drawn.unplaced))
        drawn.items.insert(item.event.id, item);
    return drawn;
}

QList<Contradiction> Chart::contradictionsOf(const Chart &chart, qint64 event) {
    return chart.contradictions
           | std::views::filter([event](const Contradiction &contradiction) { return contradiction.events.contains(event); })
           | std::ranges::to<QList<Contradiction>>();
}
