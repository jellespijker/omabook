#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringView>
#include <QtTypes>
#include <expected>
#include <optional>

// When an event happens in its story's world (#41): the value of an event's
// "when" fact, kept as JSON (see written()), each with the moment the book
// revealed it, as every fact of the graph is (graph.h). This is story time,
// the chronology, not the order a reader comes to things in.
//
//   {"calendar": "gregorian", "at": "1944-06-06", "said": "D-Day"}
//   {"calendar": "gregorian", "from": "1939-09-01", "to": "1945-09-02"}
//   {"calendar": "Third Age", "at": "3019-03-25",
//    "also": [{"calendar": "Shire Reckoning", "at": "1419-03-25"}]}
//   {"calendar": "the Long Night", "at": "3", "approximate": true,
//    "said": "the third year of the Long Night"}
//   {"said": "long before the war"}
//
// - "calendar": "gregorian" for the real one (astronomical years: 0 is 1 BC,
//   -43 is 44 BC), or the name of the story's own. Needed with a time.
// - "at" a moment, or "from" and "to" a span (either end may be left open):
//   "Y", "Y-MM", "Y-MM-DD", "Y-MM-DDTHH" or "Y-MM-DDTHH:MM". How much is
//   given is its precision, from a year to a minute.
// - "also": the same time on other calendars, when the book says so; that is
//   how one calendar is tied to another.
// - "approximate": the book says "around", "some time in".
// - "said": the book's own few words for it. With nothing else, it is a time
//   the book gives no date for ("long ago"): precision none.
//
// An event of which only the order is known has no "when" at all; its
// relations to other events say it (chronology.h).
class EventTime {
public:
    enum class Precision { None, Year, Month, Day, Hour, Minute };

    // A moment on a calendar, given as far as its precision.
    struct Point {
        qint64 year = 0;
        std::optional<int> month, day, hour, minute;

        [[nodiscard]] Precision precision() const;
        // As written: "1944-06-06", "3", "-43-03-15T11".
        [[nodiscard]] QString text() const;
        // Whether this is not later than `other` as far as both are given.
        [[nodiscard]] bool notAfter(const Point &other) const;
        [[nodiscard]] static std::expected<Point, QString> parse(QStringView text, bool gregorian);
    };

    // A time on one calendar: a moment, or a span with one or both ends.
    struct Reckoning {
        QString calendar;
        std::optional<Point> at, from, to;

        [[nodiscard]] bool any() const { return at || from || to; }
        [[nodiscard]] bool gregorian() const { return calendar == gregorianName(); }
    };

    [[nodiscard]] static QString gregorianName() { return QStringLiteral("gregorian"); }

    Reckoning on;
    QList<Reckoning> also;
    bool approximate = false;
    QString said;

    // How precise it is: of the moment, or of the coarser end of a span;
    // none when the book only says it in words.
    [[nodiscard]] Precision precision() const;
    [[nodiscard]] bool span() const { return !on.at && (on.from || on.to); }

    // The value as it is kept: compact JSON, its keys in a fixed order, so
    // the same time is always the same fact.
    [[nodiscard]] QString written() const;
    [[nodiscard]] QJsonObject toJson() const;

    // Reads a value, saying what is wrong with it when it isn't one.
    [[nodiscard]] static std::expected<EventTime, QString> parse(const QJsonObject &object);
    [[nodiscard]] static std::expected<EventTime, QString> parse(QStringView json);
    // `json` as it is kept, or empty when it is no time.
    [[nodiscard]] static QString kept(QStringView json);

    [[nodiscard]] static QString name(Precision precision);
};
