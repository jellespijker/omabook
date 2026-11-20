#include "eventtime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <array>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

// The words of a book kept with a time: a few, never a passage.
constexpr qsizetype mostSaid = 160;

[[nodiscard]] bool leap(qint64 year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

[[nodiscard]] int daysIn(qint64 year, int month) {
    constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap(year) ? 29 : days.at(static_cast<std::size_t>(month - 1));
}

// A part of a point that is there, checked against its range.
[[nodiscard]] std::expected<std::optional<int>, QString> part(const QRegularExpressionMatch &match, int group, int least, int most,
                                                              const QString &what) {
    if (!match.hasCaptured(group))
        return std::nullopt;
    const int value = match.captured(group).toInt();
    if (value < least || value > most)
        return std::unexpected(u"%1 %2 is out of range (%3 to %4)"_s.arg(what).arg(value).arg(least).arg(most));
    return value;
}

[[nodiscard]] std::expected<std::optional<EventTime::Point>, QString> pointAt(const QJsonObject &object, const QString &key,
                                                                              bool gregorian) {
    if (!object.contains(key))
        return std::nullopt;
    if (!object.value(key).isString())
        return std::unexpected(u"\"%1\" is not text"_s.arg(key));
    const auto point = EventTime::Point::parse(object.value(key).toString(), gregorian);
    if (!point)
        return std::unexpected(u"\"%1\": %2"_s.arg(key, point.error()));
    return *point;
}

// The calendar and the moment or span of a time, from its object.
[[nodiscard]] std::expected<EventTime::Reckoning, QString> reckoning(const QJsonObject &object) {
    EventTime::Reckoning reckoned;
    if (object.contains("calendar"_L1) && !object.value("calendar"_L1).isString())
        return std::unexpected(u"\"calendar\" is not text"_s);
    reckoned.calendar = object.value("calendar"_L1).toString().simplified();
    if (reckoned.calendar.compare(EventTime::gregorianName(), Qt::CaseInsensitive) == 0)
        reckoned.calendar = EventTime::gregorianName();
    const bool gregorian = reckoned.gregorian();
    for (const auto &[key, point] : {std::pair{u"at"_s, &reckoned.at}, {u"from"_s, &reckoned.from}, {u"to"_s, &reckoned.to}}) {
        auto read = pointAt(object, key, gregorian);
        if (!read)
            return std::unexpected(read.error());
        *point = *read;
    }
    if (reckoned.at && (reckoned.from || reckoned.to))
        return std::unexpected(u"a time is a moment (\"at\") or a span (\"from\", \"to\"), not both"_s);
    if (reckoned.from && reckoned.to && !reckoned.from->notAfter(*reckoned.to))
        return std::unexpected(u"the span ends (%1) before it begins (%2)"_s.arg(reckoned.to->text(), reckoned.from->text()));
    if (reckoned.any() && reckoned.calendar.isEmpty())
        return std::unexpected(u"a time needs its \"calendar\" (\"gregorian\" for the real one)"_s);
    return reckoned;
}

[[nodiscard]] std::optional<QString> unknownKey(const QJsonObject &object, const QSet<QString> &keys) {
    for (const QString &key : object.keys())
        if (!keys.contains(key))
            return key;
    return std::nullopt;
}

// The same time on other calendars than `on`'s.
[[nodiscard]] std::expected<QList<EventTime::Reckoning>, QString> others(const QJsonObject &object, const EventTime::Reckoning &on) {
    QList<EventTime::Reckoning> also;
    if (object.contains("also"_L1) && !object.value("also"_L1).isArray())
        return std::unexpected(u"\"also\" is not a list"_s);
    for (const auto &value : object.value("also"_L1).toArray()) {
        const QJsonObject other = value.toObject();
        if (const auto key = unknownKey(other, {u"calendar"_s, u"at"_s, u"from"_s, u"to"_s}))
            return std::unexpected(u"\"also\": \"%1\" is not part of a time on another calendar"_s.arg(*key));
        auto reckoned = reckoning(other);
        if (!reckoned)
            return std::unexpected(u"\"also\": "_s + reckoned.error());
        if (!reckoned->any() || reckoned->calendar == on.calendar)
            return std::unexpected(u"\"also\" is the same time on another calendar, with its moment or span"_s);
        also << std::move(*reckoned);
    }
    return also;
}

void write(QJsonObject &object, const EventTime::Reckoning &reckoned) {
    if (!reckoned.calendar.isEmpty())
        object.insert("calendar", reckoned.calendar);
    for (const auto &[key, point] : {std::pair{"at"_L1, &reckoned.at}, {"from"_L1, &reckoned.from}, {"to"_L1, &reckoned.to}})
        if (*point)
            object.insert(key, (*point)->text());
}

[[nodiscard]] EventTime::Precision ofReckoning(const EventTime::Reckoning &reckoned) {
    if (reckoned.at)
        return reckoned.at->precision();
    const auto from = reckoned.from ? reckoned.from->precision() : EventTime::Precision::Minute;
    const auto to = reckoned.to ? reckoned.to->precision() : EventTime::Precision::Minute;
    return reckoned.any() ? std::min(from, to) : EventTime::Precision::None;
}

} // namespace

EventTime::Precision EventTime::Point::precision() const {
    if (minute)
        return Precision::Minute;
    if (hour)
        return Precision::Hour;
    if (day)
        return Precision::Day;
    return month ? Precision::Month : Precision::Year;
}

QString EventTime::Point::text() const {
    QString written = QString::number(year);
    const auto two = [](int value) { return QString::number(value).rightJustified(2, u'0'); };
    if (month)
        written += u'-' + two(*month);
    if (day)
        written += u'-' + two(*day);
    if (hour)
        written += u'T' + two(*hour);
    if (minute)
        written += u':' + two(*minute);
    return written;
}

bool EventTime::Point::notAfter(const Point &other) const {
    // What is given of both, from the year down: a year is not after a day in it.
    const auto upTo = [](const Point &point, Precision precision) {
        const auto given = [precision](Precision level, const std::optional<int> &value) {
            return precision >= level ? value.value_or(0) : 0;
        };
        return std::tuple{point.year, given(Precision::Month, point.month), given(Precision::Day, point.day),
                          given(Precision::Hour, point.hour), given(Precision::Minute, point.minute)};
    };
    const Precision both = std::min(precision(), other.precision());
    return upTo(*this, both) <= upTo(other, both);
}

std::expected<EventTime::Point, QString> EventTime::Point::parse(QStringView text, bool gregorian) {
    static const QRegularExpression form(u"^(-?\\d{1,6})(?:-(\\d{2})(?:-(\\d{2})(?:T(\\d{2})(?::(\\d{2}))?)?)?)?$"_s);
    const QRegularExpressionMatch match = form.matchView(text);
    if (!match.hasMatch())
        return std::unexpected(u"\"%1\" is not a time (Y, Y-MM, Y-MM-DD, Y-MM-DDTHH or Y-MM-DDTHH:MM)"_s.arg(text));
    Point point{.year = match.captured(1).toLongLong(), .month = {}, .day = {}, .hour = {}, .minute = {}};
    // A story's own calendar may have more months, and longer ones.
    const int months = gregorian ? 12 : 99;
    auto month = part(match, 2, 1, months, u"month"_s);
    if (!month)
        return std::unexpected(month.error());
    point.month = *month;
    auto day = part(match, 3, 1, gregorian && point.month ? daysIn(point.year, *point.month) : 99, u"day"_s);
    auto hour = part(match, 4, 0, 23, u"hour"_s);
    auto minute = part(match, 5, 0, 59, u"minute"_s);
    for (const auto *checked : {&day, &hour, &minute})
        if (!*checked)
            return std::unexpected(checked->error());
    point.day = *day;
    point.hour = *hour;
    point.minute = *minute;
    return point;
}

EventTime::Precision EventTime::precision() const { return ofReckoning(on); }

QJsonObject EventTime::toJson() const {
    QJsonObject object;
    write(object, on);
    QJsonArray others;
    for (const Reckoning &other : also) {
        QJsonObject written;
        write(written, other);
        others << written;
    }
    if (!others.isEmpty())
        object.insert("also", others);
    if (approximate)
        object.insert("approximate", true);
    if (!said.isEmpty())
        object.insert("said", said);
    return object;
}

QString EventTime::written() const { return QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact)); }

std::expected<EventTime, QString> EventTime::parse(const QJsonObject &object) {
    if (const auto key = unknownKey(object, {u"calendar"_s, u"at"_s, u"from"_s, u"to"_s, u"also"_s, u"approximate"_s, u"said"_s}))
        return std::unexpected(u"\"%1\" is not part of a time"_s.arg(*key));
    EventTime time;
    auto on = reckoning(object);
    if (!on)
        return std::unexpected(on.error());
    time.on = std::move(*on);
    if (object.contains("approximate"_L1) && !object.value("approximate"_L1).isBool())
        return std::unexpected(u"\"approximate\" is not true or false"_s);
    time.approximate = object.value("approximate"_L1).toBool();
    if (object.contains("said"_L1) && !object.value("said"_L1).isString())
        return std::unexpected(u"\"said\" is not text"_s);
    time.said = object.value("said"_L1).toString().simplified();
    if (time.said.size() > mostSaid)
        return std::unexpected(u"\"said\" keeps a few of the book's words, at most %1 characters"_s.arg(mostSaid));
    if (!time.on.any() && time.said.isEmpty())
        return std::unexpected(u"a time has a moment (\"at\"), a span (\"from\", \"to\") or the book's words for it (\"said\")"_s);
    auto also = others(object, time.on);
    if (!also)
        return std::unexpected(also.error());
    time.also = std::move(*also);
    if (!time.also.isEmpty() && !time.on.any())
        return std::unexpected(u"\"also\" ties one calendar to another: the time needs its own moment or span"_s);
    return time;
}

std::expected<EventTime, QString> EventTime::parse(QStringView json) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (!document.isObject())
        return std::unexpected(document.isNull() ? u"not JSON: "_s + error.errorString() : u"a time is a JSON object"_s);
    return parse(document.object());
}

QString EventTime::kept(QStringView json) {
    const auto time = parse(json);
    return time ? time->written() : QString();
}

QString EventTime::name(Precision precision) {
    switch (precision) {
    case Precision::None:
        return u"none"_s;
    case Precision::Year:
        return u"year"_s;
    case Precision::Month:
        return u"month"_s;
    case Precision::Day:
        return u"day"_s;
    case Precision::Hour:
        return u"hour"_s;
    case Precision::Minute:
        return u"minute"_s;
    }
    return {};
}
