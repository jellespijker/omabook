#include "calendars.h"

#include <QSet>
#include <algorithm>
#include <array>
#include <utility>

using Chronology::Calendars;
using Chronology::Extent;
using Precision = EventTime::Precision;

namespace {

constexpr qint64 minutesInDay = qint64{24} * 60;

[[nodiscard]] bool leap(qint64 year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

[[nodiscard]] int daysIn(qint64 year, int month) {
    constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap(year) ? 29 : days.at(static_cast<std::size_t>(month - 1));
}

// Days from the start of the year 0 of the real calendar, proleptic and with
// astronomical years (Howard Hinnant's days_from_civil, with its epoch moved).
[[nodiscard]] qint64 civilDays(qint64 year, int month, int day) {
    if (month <= 2)
        --year;
    const qint64 era = (year >= 0 ? year : year - 399) / 400;
    const qint64 ofEra = year - era * 400;
    const qint64 ofYear = ((153 * (month > 2 ? month - 3 : month + 9)) + 2) / 5 + day - 1;
    return era * 146097 + ofEra * 365 + ofEra / 4 - ofEra / 100 + ofYear + 60;
}

// The point as far as `precision`, the rest left out.
[[nodiscard]] EventTime::Point cut(EventTime::Point point, Precision precision) {
    if (precision < Precision::Month)
        point.month.reset();
    if (precision < Precision::Day)
        point.day.reset();
    if (precision < Precision::Hour)
        point.hour.reset();
    if (precision < Precision::Minute)
        point.minute.reset();
    return point;
}

// How much two moments that are the same time have alike below the year:
// what a tie between their calendars carries over.
[[nodiscard]] Precision alike(const EventTime::Point &a, const EventTime::Point &b) {
    const std::array<std::pair<const std::optional<int> *, const std::optional<int> *>, 4> parts{
        {{&a.month, &b.month}, {&a.day, &b.day}, {&a.hour, &b.hour}, {&a.minute, &b.minute}}};
    Precision carried = Precision::Year;
    for (const auto &[mine, theirs] : parts) {
        if (!*mine || *mine != *theirs)
            break;
        carried = static_cast<Precision>(static_cast<int>(carried) + 1);
    }
    return carried;
}

// The point a time is tied by: its moment, or its span's first end, else its last.
[[nodiscard]] std::optional<EventTime::Point> anchor(const EventTime::Reckoning &reckoned) {
    if (reckoned.at)
        return reckoned.at;
    return reckoned.from ? reckoned.from : reckoned.to;
}

[[nodiscard]] EventTime::Reckoning moved(const EventTime::Reckoning &reckoned, const QString &calendar, const Calendars::Tie &tie) {
    EventTime::Reckoning there{.calendar = calendar, .at = {}, .from = {}, .to = {}};
    const auto move = [&tie](const std::optional<EventTime::Point> &point) -> std::optional<EventTime::Point> {
        if (!point)
            return std::nullopt;
        EventTime::Point put = cut(*point, tie.carried);
        put.year += tie.years;
        return put;
    };
    there.at = move(reckoned.at);
    there.from = move(reckoned.from);
    there.to = move(reckoned.to);
    return there;
}

void widen(Calendars::Shape &shape, const EventTime::Reckoning &reckoned) {
    for (const auto *point : {&reckoned.at, &reckoned.from, &reckoned.to})
        if (*point) {
            shape.months = std::max(shape.months, (*point)->month.value_or(1));
            shape.days = std::max(shape.days, (*point)->day.value_or(1));
        }
}

} // namespace

Calendars::Calendars(const QList<Dated> &times) {
    for (const Dated &dated : times) {
        if (!dated.time.on.any())
            continue;
        widen(m_shapes[dated.time.on.calendar], dated.time.on);
        for (const EventTime::Reckoning &other : dated.time.also) {
            widen(m_shapes[other.calendar], other);
            tie(dated, other);
        }
    }
    // Gone through by name, so that the same times always reach the same way.
    for (auto &links : m_links)
        std::ranges::sort(links, {}, &Link::to);
}

void Calendars::tie(const Dated &dated, const EventTime::Reckoning &other) {
    const EventTime::Reckoning &on = dated.time.on;
    const auto here = anchor(on);
    const auto there = anchor(other);
    if (!here || !there)
        return;
    const Tie toHere{.years = here->year - there->year, .carried = alike(*here, *there)};
    const auto pair = std::minmax(on.calendar, other.calendar);
    const QString key = pair.first + u'\n' + pair.second;
    QList<Link> &links = m_links[on.calendar];
    const auto known = std::ranges::find(links, other.calendar, &Link::to);
    if (known == links.end()) {
        links.append({.to = other.calendar, .tie = toHere});
        m_links[other.calendar].append({.to = on.calendar, .tie = {.years = -toHere.years, .carried = toHere.carried}});
        m_tiedBy.insert(key, dated);
        return;
    }
    if (known->tie.years != toHere.years) {
        m_disagreements.append({.kept = m_tiedBy.value(key), .other = dated});
        return;
    }
    // The same tie told again, perhaps to the day where it was to the year.
    const Precision carried = std::max(known->tie.carried, toHere.carried);
    known->tie.carried = carried;
    std::ranges::find(m_links[other.calendar], on.calendar, &Link::to)->tie.carried = carried;
}

Calendars::Scale Calendars::scale(const QString &calendar) const {
    Scale scale;
    scale.m_calendar = calendar;
    scale.m_shape = m_shapes.value(calendar);
    scale.m_ties.insert(calendar, {});
    // Breadth first from the axis, each calendar by its nearest tie.
    QList<QString> frontier{calendar};
    while (!frontier.isEmpty()) {
        QList<QString> next;
        for (const QString &here : std::as_const(frontier))
            for (const Link &link : m_links.value(here)) {
                if (scale.m_ties.contains(link.to))
                    continue;
                const Tie toAxis = scale.m_ties.value(here);
                scale.m_ties.insert(link.to,
                                    {.years = link.tie.years + toAxis.years, .carried = std::min(link.tie.carried, toAxis.carried)});
                next.append(link.to);
            }
        frontier = std::move(next);
    }
    return scale;
}

std::optional<EventTime::Reckoning> Calendars::Scale::on(const EventTime &time) const {
    if (time.on.calendar == m_calendar)
        return time.on;
    for (const EventTime::Reckoning &other : time.also)
        if (other.calendar == m_calendar)
            return other;
    if (const auto tie = m_ties.constFind(time.on.calendar); tie != m_ties.cend())
        return moved(time.on, m_calendar, *tie);
    return std::nullopt;
}

std::optional<Extent> Calendars::Scale::extent(const EventTime &time) const {
    const auto reckoned = time.on.any() ? on(time) : std::nullopt;
    if (!reckoned)
        return std::nullopt;
    if (reckoned->at)
        return Extent{.from = start(*reckoned->at), .to = end(*reckoned->at)};
    return Extent{.from = reckoned->from ? std::optional(start(*reckoned->from)) : std::nullopt,
                  .to = reckoned->to ? std::optional(end(*reckoned->to)) : std::nullopt};
}

qint64 Calendars::Scale::start(const EventTime::Point &point) const {
    // A part put on this calendar from another may not fit it; it is held to it.
    const bool real = m_calendar == EventTime::gregorianName();
    const int month = std::clamp(point.month.value_or(1), 1, real ? 12 : m_shape.months);
    const int day = std::clamp(point.day.value_or(1), 1, real ? daysIn(point.year, month) : m_shape.days);
    const qint64 days = real ? civilDays(point.year, month, day) : ((point.year * m_shape.months) + month - 1) * m_shape.days + day - 1;
    return days * minutesInDay + qint64{std::clamp(point.hour.value_or(0), 0, 23)} * 60 + std::clamp(point.minute.value_or(0), 0, 59);
}

qint64 Calendars::Scale::end(const EventTime::Point &point) const {
    const int months = m_calendar == EventTime::gregorianName() ? 12 : m_shape.months;
    switch (point.precision()) {
    case Precision::None:
    case Precision::Year:
        return start({.year = point.year + 1, .month = {}, .day = {}, .hour = {}, .minute = {}});
    case Precision::Month: {
        const int month = point.month.value_or(1);
        const bool last = month >= months;
        return start({.year = last ? point.year + 1 : point.year, .month = last ? 1 : month + 1, .day = {}, .hour = {}, .minute = {}});
    }
    case Precision::Day:
        return start(point) + minutesInDay;
    case Precision::Hour:
        return start(point) + 60;
    case Precision::Minute:
        return start(point) + 1;
    }
    return start(point);
}
