#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtTypes>
#include <optional>

#include "chronology.h"
#include "eventtime.h"

// Story time as a line (#43): the times of events (eventtime.h) put on one
// calendar's axis, so that they can be compared and drawn. A calendar is
// tied to another where a book gives the same time on both ("also"), and a
// time on one can then be put on the other: by the years between them, and
// by the months and days as well where the tie keeps them alike (Third Age
// 3019-03-25 is Shire Reckoning 1419-03-25). Ties go on through other
// calendars. A calendar no tie reaches stays apart: its times are compared
// only with each other.
namespace Chronology {

// A stretch of story time on one calendar, in minutes from the start of its
// year 0: a moment is the stretch its precision leaves open (1944 is all of
// the year), a span runs from its first moment to the end of its last. An
// end left open is none, and `to` is the first minute after it.
struct Extent {
    std::optional<qint64> from, to;

    // Whether this ends before the other begins: the one comes first.
    [[nodiscard]] bool before(const Extent &other) const { return to && other.from && *to <= *other.from; }
    // Whether the two can be at the same time: neither comes first.
    [[nodiscard]] bool meets(const Extent &other) const { return !before(other) && !other.before(*this); }
};

// The calendars a set of times is on, and how the times tie them together.
class Calendars {
public:
    // How long a story's own calendar's years and months are, as far as its
    // times show: at least the twelve months and thirty days of most.
    struct Shape {
        int months = 12, days = 30;
    };
    // How a time on another calendar is put on this one: the years to add,
    // and how much of the rest carries over.
    struct Tie {
        qint64 years = 0;
        EventTime::Precision carried = EventTime::Precision::Minute;
    };

    // One calendar as an axis, and every calendar tied to it.
    class Scale {
    public:
        [[nodiscard]] const QString &calendar() const { return m_calendar; }
        [[nodiscard]] bool reaches(const QString &calendar) const { return m_ties.contains(calendar); }
        // The time as it is on this calendar: as the book gave it there, or
        // put on it through a tie. None when no tie reaches it.
        [[nodiscard]] std::optional<EventTime::Reckoning> on(const EventTime &time) const;
        // Where on the axis it is; none when it isn't on it, or is no moment
        // or span (only the book's words).
        [[nodiscard]] std::optional<Extent> extent(const EventTime &time) const;

    private:
        friend class Calendars;
        [[nodiscard]] qint64 start(const EventTime::Point &point) const;
        [[nodiscard]] qint64 end(const EventTime::Point &point) const;

        QString m_calendar;
        Shape m_shape;
        QHash<QString, Tie> m_ties; // of each calendar that reaches this one, itself too
    };

    // Two times that tie the same calendars apart by different years: the
    // first revealed is kept, the other set aside.
    struct Disagreement {
        Dated kept, other;
    };

    Calendars() = default;
    // The calendars of these times; read in the order given, which decides
    // which of two disagreeing ties is kept.
    explicit Calendars(const QList<Dated> &times);

    [[nodiscard]] Scale scale(const QString &calendar) const;
    [[nodiscard]] bool tied(const QString &a, const QString &b) const { return a == b || scale(a).reaches(b); }
    [[nodiscard]] const QList<Disagreement> &disagreements() const { return m_disagreements; }

private:
    struct Link {
        QString to;
        Tie tie; // how a time on `to` is put on this one
    };

    void tie(const Dated &dated, const EventTime::Reckoning &other);

    QHash<QString, Shape> m_shapes;
    QHash<QString, QList<Link>> m_links;
    QHash<QString, Dated> m_tiedBy; // the time that tied a pair, by "a\nb"
    QList<Disagreement> m_disagreements;
};

} // namespace Chronology
