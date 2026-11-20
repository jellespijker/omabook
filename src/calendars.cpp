#include "calendars.h"

#include <QSet>
#include <algorithm>
#include <array>
#include <functional>
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

// Rounded down, also for a minute before the year 0.
[[nodiscard]] qint64 floorDiv(qint64 value, qint64 by) { return value / by - (value % by != 0 && (value < 0) != (by < 0) ? 1 : 0); }

// The day of the real calendar so many days from the start of its year 0:
// civilDays() the other way round (Hinnant's civil_from_days).
[[nodiscard]] EventTime::Point civil(qint64 days) {
    const qint64 fromMarch = days - 60;
    const qint64 era = (fromMarch >= 0 ? fromMarch : fromMarch - 146096) / 146097;
    const qint64 ofEra = fromMarch - era * 146097;
    const qint64 yearOfEra = (ofEra - ofEra / 1460 + ofEra / 36524 - ofEra / 146096) / 365;
    const qint64 ofYear = ofEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const qint64 shifted = (5 * ofYear + 2) / 153;
    const int month = static_cast<int>(shifted < 10 ? shifted + 3 : shifted - 9);
    return {.year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0),
            .month = month,
            .day = static_cast<int>(ofYear - (153 * shifted + 2) / 5 + 1),
            .hour = {},
            .minute = {}};
}

// A tick as far as it needs: the first month of a year as the year,
// midnight as its day.
// Whether a point's last part is the first of its unit: month or day 1, hour or minute 0.
[[nodiscard]] bool first(const EventTime::Point &point, Precision unit) {
    switch (unit) {
    case Precision::Minute:
        return point.minute.value_or(0) == 0;
    case Precision::Hour:
        return point.hour.value_or(0) == 0;
    case Precision::Day:
        return point.day.value_or(1) == 1;
    case Precision::Month:
        return point.month.value_or(1) == 1;
    case Precision::None:
    case Precision::Year:
        break;
    }
    return false;
}

[[nodiscard]] EventTime::Point roundest(EventTime::Point point, Precision unit) {
    point = cut(point, unit);
    while (unit > Precision::Year) {
        if (!first(point, unit))
            break;
        unit = static_cast<Precision>(static_cast<int>(unit) - 1);
        point = cut(point, unit);
    }
    return point;
}

// A ruler's step: so many of a unit (a year, a month, ... a minute).
struct Step {
    Precision unit = Precision::Year;
    qint64 count = 1;
};

// The steps a ruler can go by, the finest first.
[[nodiscard]] QList<Step> steps() {
    QList<Step> all;
    for (const qint64 count : {1, 5, 10, 15, 30})
        all.append({.unit = Precision::Minute, .count = count});
    for (const qint64 count : {1, 3, 6, 12})
        all.append({.unit = Precision::Hour, .count = count});
    for (const qint64 count : {1, 2, 5, 10})
        all.append({.unit = Precision::Day, .count = count});
    for (const qint64 count : {1, 3, 6})
        all.append({.unit = Precision::Month, .count = count});
    for (qint64 tens = 1; tens <= 1'000'000'000; tens *= 10)
        for (const qint64 times : {1, 2, 5})
            all.append({.unit = Precision::Year, .count = tens * times});
    return all;
}

// The finest step a ruler of `most` ticks can go by over `minutes`, on a
// calendar with years of `days` days and `months` months.
[[nodiscard]] Step stepFor(double minutes, int most, double days, int months) {
    const double year = days * minutesInDay;
    const auto length = [&](const Step &step) {
        double unit = 1;
        switch (step.unit) {
        case Precision::None:
        case Precision::Year:
            unit = year;
            break;
        case Precision::Month:
            unit = year / months;
            break;
        case Precision::Day:
            unit = static_cast<double>(minutesInDay);
            break;
        case Precision::Hour:
            unit = 60;
            break;
        case Precision::Minute:
            break;
        }
        return unit * static_cast<double>(step.count);
    };
    const QList<Step> all = steps();
    const auto fits = std::ranges::find_if(all, [&](const Step &step) { return minutes / length(step) <= most; });
    return fits != all.end() ? *fits : all.last();
}

// An hour or a minute: every so many from the start of a day, which they divide.
[[nodiscard]] QList<Calendars::Scale::Tick> clockTicks(const Calendars::Scale &scale, qint64 from, qint64 to, int most, const Step &step) {
    QList<Calendars::Scale::Tick> ticks;
    const qint64 by = (step.unit == Precision::Hour ? 60 : 1) * step.count;
    for (qint64 minute = -floorDiv(-from, by) * by; minute <= to && ticks.size() <= qsizetype{2} * most; minute += by)
        ticks.append({.minute = minute, .point = roundest(scale.point(minute), step.unit)});
    return ticks;
}

// How long a calendar's years and months are: so many months, of so many days each.
struct Calendar {
    int months = 12;
    std::function<int(qint64, int)> days;
};

// The round moment a step of years, months or days after `at`.
[[nodiscard]] EventTime::Point advanced(EventTime::Point at, const Step &step, const Calendar &calendar) {
    const auto nextMonth = [&calendar](EventTime::Point &point, int by) {
        const int month = point.month.value_or(1) + by;
        point.year += (month - 1) / calendar.months;
        point.month = (month - 1) % calendar.months + 1;
    };
    const int count = static_cast<int>(step.count);
    if (step.unit == Precision::Year) {
        at.year += step.count;
    } else if (step.unit == Precision::Month) {
        nextMonth(at, count);
    } else {
        // Every so many days from the first, none so near the end of the
        // month that it runs into the next month's first.
        const int day = at.day.value_or(1) + count;
        const bool over = day > calendar.days(at.year, at.month.value_or(1)) - count / 2;
        at.day = over ? 1 : day;
        if (over)
            nextMonth(at, 1);
    }
    return at;
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

EventTime::Point Calendars::Scale::point(qint64 minute) const {
    const qint64 days = floorDiv(minute, minutesInDay);
    const qint64 rest = minute - days * minutesInDay;
    EventTime::Point point;
    if (real()) {
        point = civil(days);
    } else {
        const qint64 perYear = qint64{m_shape.months} * m_shape.days;
        point.year = floorDiv(days, perYear);
        const qint64 ofYear = days - point.year * perYear;
        point.month = static_cast<int>(ofYear / m_shape.days) + 1;
        point.day = static_cast<int>(ofYear % m_shape.days) + 1;
    }
    point.hour = static_cast<int>(rest / 60);
    point.minute = static_cast<int>(rest % 60);
    return point;
}

QList<Calendars::Scale::Tick> Calendars::Scale::ticks(qint64 from, qint64 to, int most) const {
    if (to <= from || most < 1)
        return {};
    const int months = real() ? 12 : m_shape.months;
    const Step step = stepFor(static_cast<double>(to - from), most, real() ? 365.2425 : static_cast<double>(months) * m_shape.days, months);
    if (step.unit == Precision::Hour || step.unit == Precision::Minute)
        return clockTicks(*this, from, to, most, step);
    QList<Tick> ticks;
    // From the start of the year or the month `from` is in, so that the steps are round.
    EventTime::Point at = cut(point(from), step.unit == Precision::Day ? Precision::Month : Precision::Year);
    if (step.unit == Precision::Year)
        at.year = floorDiv(at.year, step.count) * step.count;
    const Calendar shape{.months = months, .days = [this](qint64 year, int month) { return real() ? daysIn(year, month) : m_shape.days; }};
    for (qint64 minute = start(at); minute <= to && ticks.size() <= qsizetype{2} * most; minute = start(at)) {
        if (minute >= from)
            ticks.append({.minute = minute, .point = roundest(at, step.unit)});
        at = advanced(at, step, shape);
    }
    return ticks;
}
