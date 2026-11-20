#include "chronologyprint.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include "chronologyexport.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto gregorian = "gregorian"_L1;

// What a part of the chronology needs to say who and where: the books'
// titles, and every event's name.
struct Names {
    QJsonObject books;
    QHash<qint64, QString> events;

    [[nodiscard]] QString book(qint64 id) const { return books.value(QString::number(id)).toString(u"book %1"_s.arg(id)); }
    // Where something was said: "The Long Siege, section 3", or "by hand".
    [[nodiscard]] QString where(const QJsonObject &revealed) const {
        const qint64 id = revealed.value("book").toInteger();
        return id == 0 ? u"by hand"_s : u"%1, section %2"_s.arg(book(id)).arg(revealed.value("section").toInt());
    }
};

[[nodiscard]] QString calendarName(const QString &calendar) {
    return calendar == gregorian ? u"the Gregorian calendar"_s : u"the %1 calendar"_s.arg(calendar);
}

[[nodiscard]] QString timeText(const QJsonObject &time, const QString &calendar) { return ChronologyExport::said(time, calendar); }

[[nodiscard]] QString laneName(const QJsonObject &lane, const Names &names) {
    const QJsonObject line = lane.value("timeline").toObject();
    return line.value("id").toInteger() == 0 ? names.book(line.value("book").toInteger()) + u"'s own order"_s
                                             : line.value("name").toString();
}

// A statement of a contradiction: a relation as it was written, or an
// event's time.
[[nodiscard]] QString statement(const QJsonObject &source, const Names &names) {
    QString said = source.value("said").toString();
    if (!said.startsWith(u'{'))
        return said;
    return names.events.value(source.value("event").toInteger()) + u": "_s
           + timeText(QJsonDocument::fromJson(said.toUtf8()).object(), gregorian);
}

[[nodiscard]] QStringList contradictionLines(const QJsonObject &contradiction, const Names &names) {
    QStringList lines{u"  ! "_s + contradiction.value("rule").toString()};
    const QJsonObject aside = contradiction.value("set_aside").toObject();
    for (const auto &value : contradiction.value("sources").toArray()) {
        const QJsonObject source = value.toObject();
        const bool setAside = source.value("edge") == aside.value("edge") && source.value("fact") == aside.value("fact");
        lines << u"      %1  (%2)%3"_s.arg(statement(source, names), names.where(source.value("revealed").toObject()),
                                           setAside ? u"  set aside"_s : QString());
    }
    return lines;
}

[[nodiscard]] QStringList laneLines(const QJsonObject &lane, const Names &names) {
    const QJsonObject line = lane.value("timeline").toObject();
    const QString calendar = lane.value("calendar").toString();
    QString head = laneName(lane, names);
    if (line.value("id").toInteger() != 0)
        head += u" ("_s + line.value("whose").toString() + u")"_s;
    if (!calendar.isEmpty())
        head += u", on "_s + calendarName(calendar);
    QStringList lines{head};
    for (const auto &value : lane.value("items").toArray()) {
        const QJsonObject item = value.toObject();
        QString text = u"  %1  %2  %3"_s.arg(item.value("rank").toInt() + 1, 3)
                           .arg(timeText(item.value("time").toObject(), calendar), -24)
                           .arg(item.value("name").toString());
        if (const qint64 within = item.value("within").toInteger(); within != 0)
            text += u"  during "_s + names.events.value(within);
        if (item.contains("revealed"))
            text += u"  ("_s + names.where(item.value("revealed").toObject()) + u")"_s;
        lines << text;
    }
    for (const auto &value : lane.value("contradictions").toArray())
        lines += contradictionLines(value.toObject(), names);
    return lines;
}

[[nodiscard]] QStringList partLines(const QJsonObject &part, const Names &given) {
    Names names = given;
    const QJsonArray lanes = part.value("lanes").toArray();
    for (const auto &lane : lanes)
        for (const auto &item : lane.toObject().value("items").toArray())
            names.events.insert(item.toObject().value("event").toInteger(), item.toObject().value("name").toString());
    for (const auto &item : part.value("unplaced").toArray())
        names.events.insert(item.toObject().value("event").toInteger(), item.toObject().value("name").toString());
    QStringList lines;
    for (const auto &lane : lanes)
        lines += laneLines(lane.toObject(), names) << QString();
    for (const auto &value : part.value("axes").toArray()) {
        QStringList together;
        for (const auto &at : value.toObject().value("lanes").toArray())
            together << laneName(lanes.at(at.toInt()).toObject(), names);
        if (together.size() > 1)
            lines << u"Lined up on one axis: "_s + together.join(u", "_s) << QString();
    }
    if (const QJsonArray unplaced = part.value("unplaced").toArray(); !unplaced.isEmpty()) {
        lines << u"Not placed yet: no time and no order is known of"_s;
        for (const auto &value : unplaced)
            lines << u"  "_s + value.toObject().value("name").toString() + u"  "_s
                         + timeText(value.toObject().value("time").toObject(), gregorian);
        lines << QString();
    }
    if (const QJsonArray contradictions = part.value("contradictions").toArray(); !contradictions.isEmpty()) {
        lines << u"Between calendars:"_s;
        for (const auto &value : contradictions)
            lines += contradictionLines(value.toObject(), names);
        lines << QString();
    }
    return lines;
}

[[nodiscard]] QString sideName(const QJsonArray &side, const std::function<QString(qint64)> &collection) {
    QStringList named;
    for (const auto &top : side)
        named << (top.toInteger() == 0 ? u"no collection"_s : collection(top.toInteger()));
    return named.isEmpty() ? QString() : u"Of the books in "_s + named.join(u" and "_s) + u":"_s;
}

[[nodiscard]] QString cutText(const QJsonObject &cut, const Names &names) {
    const QString at = cut.value("at").toString();
    if (at == "whole"_L1)
        return u"Everything known, read or not."_s;
    if (at == "place"_L1)
        return u"Cut at section %1 of %2."_s.arg(cut.value("section").toInt()).arg(names.book(cut.value("book").toInteger()));
    return u"Cut where you are in each book."_s;
}

} // namespace

QString ChronologyPrint::text(const QJsonObject &answer, const std::function<QString(qint64)> &collection) {
    const Names names{.books = answer.value("books").toObject(), .events = {}};
    const QJsonArray parts = answer.value("parts").toArray();
    QStringList lines;
    for (const auto &part : parts) {
        if (parts.size() > 1)
            lines << sideName(part.toObject().value("side").toArray(), collection);
        lines += partLines(part.toObject(), names);
    }
    const QJsonObject events = answer.value("events").toObject();
    const qint64 shown = events.value("shown").toInteger();
    const qint64 total = events.value("total").toInteger();
    if (total == 0)
        lines << u"Nothing is known yet of when these events happen."_s;
    else if (shown < total)
        lines << u"%1 of %2 events."_s.arg(shown).arg(total);
    else
        lines << u"%1 events."_s.arg(total);
    lines.last() += u" "_s + cutText(answer.value("cut").toObject(), names);
    return lines.join(u'\n');
}

QString ChronologyPrint::written(const QJsonObject &answer) {
    const QString name = answer.value("name").toString();
    if (answer.contains("fact"))
        return u"%1: %2, by hand."_s.arg(name, timeText(answer.value("time").toObject(), gregorian));
    if (answer.contains("relation"))
        return u"%1 %2 %3, by hand."_s.arg(name, answer.value("relation").toString(), answer.value("other_name").toString());
    if (answer.contains("timeline_name"))
        return u"%1 is on %2, by hand."_s.arg(name, answer.value("timeline_name").toString());
    return u"Timeline %1: %2 (%3)."_s.arg(answer.value("timeline").toInteger()).arg(name, answer.value("whose").toString());
}
