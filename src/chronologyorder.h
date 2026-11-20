#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTypes>
#include <optional>
#include <tuple>

#include "calendars.h"
#include "chronology.h"
#include "graph.h"

// Events in the order they happen (#43): from the times, the relations of
// order and the timelines that are known (chronology.h), one order for a
// set of events, with where each is on its calendar when that is known.
// Pure: it reads only the facts it is given, which are cut where the reader
// is already (chronicle.h reads them so), so it never says more than that.
//
// How an order is made:
//
// - One calendar is the order's axis: the one most of its events can be put
//   on (calendars.h), the real one first at a tie, then by name. Their times
//   place them there, and an event that ends before another begins comes
//   first.
// - The relations place the rest, and order events whose times overlap:
//   "before" and "after" one after the other, "same time as" together (they
//   share a rank), "during" inside the other (after it begins, before what
//   follows it, within its time where that is known).
// - An event of which only relations are known goes as near its neighbours
//   as they allow: after the latest that comes before it, else just before
//   the first that comes after it.
// - Whatever the relations leave open goes by time, then by where it was
//   first met in the books, then by its number: the same facts always give
//   the same order, whatever order they were learned in.
//
// What can't all be true is never hidden. Each contradiction is reported,
// with every statement in it and where each was revealed, and resolved by
// one rule so that there is still an order:
//
// - **Dates win over relations.** A relation that a date contradicts (one
//   event "before" another that ends before it begins), directly or through
//   a chain of relations, is set aside.
// - **The later revealed wins** among relations that go round in a circle
//   (A before B before A), or with dates (each the shortest such circle):
//   the earliest revealed of them is set aside, as a
//   flashback corrects what was thought before. What no book says (put in
//   by hand) counts as revealed last.
// - **One time per event**: within a book the last revealed (a later page
//   refines an earlier one); across books what was put in by hand, else the
//   most precise, else the exact over the approximate, else the first book's
//   (chosen()). A time of another book that disagrees with it is reported.
namespace Chronology {

// What a chronology is made from: the events, the timelines and what is
// known of them, as the reader knows them (chronicle.h: read()).
struct Facts {
    QList<GraphNode> events, timelines;
    QHash<qint64, QList<Dated>> times;    // each event's times, in the order revealed
    QHash<qint64, QList<GraphPlace>> met; // where each event was met, in the books the reader knows it from
    // The relations of order between events, as they were written
    // ("before", "after", "during", "same time as"), and each event "on" its
    // timelines.
    QList<GraphEdge> edges;
};

// The order a reader comes to things in, across books: by book, then place;
// what no book says last.
using Revealed = std::tuple<bool, qint64, int, double>;
[[nodiscard]] inline Revealed revealed(const GraphPlace &place) { return {place.bookId == 0, place.bookId, place.section, place.fraction}; }

// The one time an event is placed by, of all those revealed of it (in the
// order revealed): see the rule above. None when no time of it has a moment
// or a span.
[[nodiscard]] std::optional<Dated> chosen(const QList<Dated> &times);

// Of each book, the last time it revealed with a moment or a span, by book.
[[nodiscard]] QList<Dated> latestOfEachBook(const QList<Dated> &times);

// A statement in a contradiction: a relation (its edge) or a time (its fact).
struct Source {
    qint64 edge = 0, fact = 0;
    qint64 event = 0; // the event it is of; for a relation, its first
    QString said;     // "The siege before The flood", or the time as kept
    GraphMoment revealed;

    [[nodiscard]] QJsonObject toJson() const;
};

struct Contradiction {
    enum class Kind {
        Cycle,     // relations in a circle
        Date,      // a relation, or a chain of them, against the times
        Times,     // two books give an event times that can't both be
        Calendars, // two ties between the same calendars disagree
    };
    Kind kind = Kind::Cycle;
    QList<qint64> events;
    QList<Source> sources; // all of the statements, in the order revealed
    Source setAside;       // the one not followed

    [[nodiscard]] static QString name(Kind kind);
    // The rule that set it aside, in a sentence.
    [[nodiscard]] QString rule() const;
    [[nodiscard]] QJsonObject toJson() const;
};

// An event in an order.
struct Item {
    GraphNode event;
    // Its place in the order, from 0: events at the same time share one.
    int rank = 0;
    std::optional<Dated> time;    // the time it is placed by, when it has one
    std::optional<Extent> extent; // where it is on the order's calendar, when it is on it
    qint64 within = 0;            // the event it happens during, if any

    [[nodiscard]] QJsonObject toJson() const;
};

// One order of events: a timeline's, or several on one axis.
struct Lane {
    GraphNode timeline; // id 0 for a book's own order (bookId) and for an axis's
    QString calendar;   // the axis's; empty when none of its events has a date on one
    QList<Item> items;  // in order
    QList<Contradiction> contradictions;

    [[nodiscard]] QJsonObject toJson() const;
};

// The facts read once, to make any number of orders of them.
class Orderer {
public:
    explicit Orderer(const Facts &facts);

    // The order of these events, as one lane on `timeline`.
    [[nodiscard]] Lane order(const GraphNode &timeline, QList<qint64> events) const;

    // Whether anything places an event: a time with a moment or a span, or a relation.
    [[nodiscard]] bool placed(qint64 event) const { return m_chosen.contains(event) || m_relationsOf.contains(event); }
    // The book an event was first met in; 0 for one of no book.
    [[nodiscard]] qint64 book(qint64 event) const;
    // Where an event was first met, in the order a reader comes to things.
    [[nodiscard]] Revealed met(qint64 event) const { return m_firstMet.value(event); }
    [[nodiscard]] const Calendars &calendars() const { return m_calendars; }
    [[nodiscard]] GraphNode event(qint64 id) const { return m_events.value(id); }
    // An event as an item of no order: its time, if only in words.
    [[nodiscard]] Item apart(qint64 event) const;
    // The events each relation of order ties, either way round.
    [[nodiscard]] QList<std::pair<qint64, qint64>> ties() const;

    // A relation of order as read from its first event.
    struct Relation {
        enum class Kind { Before, Same, During };
        qint64 edge = 0, first = 0, second = 0;
        Kind kind = Kind::Before;
        GraphMoment revealed;

        [[nodiscard]] static QString name(Kind kind);
    };

private:
    friend class Arrangement;

    QHash<qint64, GraphNode> m_events;
    QHash<qint64, Revealed> m_firstMet;
    QHash<qint64, QList<Dated>> m_times;
    QHash<qint64, Dated> m_chosen;
    QList<Relation> m_relations; // by edge
    QHash<qint64, int> m_byEdge; // where each relation is in m_relations
    QHash<qint64, QList<int>> m_relationsOf;
    Calendars m_calendars;
};

} // namespace Chronology
