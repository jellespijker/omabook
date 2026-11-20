#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <expected>
#include <optional>

#include "eventtime.h"
#include "graph.h"
#include "graphlayers.h"
#include "library.h"

// The chronology (#40): where the events of the books are in the time of
// their world, not in the order they are read (that is a thing's reveal
// history, Graph::timeline). It is kept as ordinary graph facts and
// relations, each with the moment it was revealed, so a reader is never told
// more of it than they have read, and it travels with the rest of the graph:
//
// - an event's time is a fact of kind "when", an EventTime (eventtime.h);
//   a later fact refines an earlier one, as a summary does;
// - its order against another event is a relation: "before", "after",
//   "during" or "same time as" (a flashback reveals that something came first);
// - a timeline is a thing of its own (kind "timeline": a book's, a series',
//   a world's, real history, or one the reader names), and an event is "on"
//   one or more of them. Two books share a timeline when their events are on
//   the same one.
//
// An event with none of these is simply not placed yet.
namespace Chronology {

[[nodiscard]] inline QString whenFact() { return QStringLiteral("when"); }
[[nodiscard]] inline QString timelineKind() { return QStringLiteral("timeline"); }
[[nodiscard]] inline QString onRelation() { return QStringLiteral("on"); }
// The relations of order, as they are written from the first event to the
// second ("the siege" before "the flood").
[[nodiscard]] const QStringList &orderRelations();
// The note on an order that was concluded rather than read in so many words.
[[nodiscard]] inline QString inferred() { return QStringLiteral("inferred"); }

// Whose a timeline is, kept in its data as {"timeline": "history"}.
enum class Whose { Book, Series, World, History, Reader };
[[nodiscard]] QString name(Whose whose);
[[nodiscard]] std::optional<Whose> whose(const QString &name);

// A time of an event, with the moment it was revealed.
struct Dated {
    EventTime time;
    GraphMoment revealed;
    qint64 fact = 0;
};

// Another event in order with this one. The relation is read from this
// event: "before", "after", "during", "includes" (the other is during this
// one) or "same time as", whichever way the relation was written.
struct Ordered {
    qint64 edge = 0;
    GraphNode other;
    QString relation;
    GraphMoment revealed;
    QString note;
};

// A timeline the event is on.
struct Placed {
    qint64 edge = 0;
    GraphNode timeline;
    GraphMoment revealed;
};

// What is known of when an event happens, cut where the reader is.
struct Known {
    GraphNode event;
    QList<Dated> times; // each "when" revealed so far, in the order revealed
    QList<Ordered> order;
    QList<Placed> timelines;

    // The latest word on its time: the last revealed.
    [[nodiscard]] std::optional<Dated> latest() const;
    [[nodiscard]] QJsonObject toJson() const;
};

// What is known of an event to whoever `known` says knows a place:
// GraphContext::readerKnows for the reader, as far as they read each book.
// Only what was revealed at a known place (or of no book) is told, and only
// of events and timelines met at one. Nothing when the event itself isn't
// known, or isn't an event.
[[nodiscard]] std::optional<Known> of(const Library &library, qint64 event, const GraphKnown &known);
// The same of an event of a subscribed space's layer, which is cut at the
// reader's place already (GraphContext::layers).
[[nodiscard]] std::optional<Known> of(const GraphLayers::Layer &layer, qint64 event);

// Writing it, each checked: the time of an event, revealed at a moment.
[[nodiscard]] std::expected<qint64, QString> setWhen(Graph &graph, qint64 event, const EventTime &time,
                                                     const std::optional<GraphMoment> &revealed = std::nullopt);
// That `event` is `relation` (one of orderRelations()) `other`, learned at a
// place, with a note (inferred(), when it was concluded).
[[nodiscard]] std::expected<qint64, QString> order(Graph &graph, qint64 event, const QString &relation, qint64 other,
                                                   const std::optional<GraphMoment> &learned = std::nullopt, const QString &note = {});
// That an event is on a timeline, learned at a place.
[[nodiscard]] std::expected<qint64, QString> placeOn(Graph &graph, qint64 event, qint64 timeline,
                                                     const std::optional<GraphMoment> &learned = std::nullopt);
// A timeline, made or found by its name: real history is one for the whole
// library, whatever it is called. `seen` is where a book first places
// something on it.
[[nodiscard]] qint64 timeline(Graph &graph, Whose whose, const QString &called, const std::optional<GraphMoment> &seen = std::nullopt);
// The timeline a book's events go on: the book's own (by its title), its
// series' (by the series), a world's (by the name it is `called`) or real
// history. A book's, a series' or a world's is shared only by the books it
// belongs to that are on the same side of the library (booksides.h): one of
// that name met only on another side is another timeline, told apart by the
// book's authors, series or title. 0 when there is no such book.
[[nodiscard]] qint64 timelineFor(Graph &graph, const Library &library, qint64 bookId, Whose whose, const QString &called,
                                 const std::optional<GraphMoment> &seen = std::nullopt);

} // namespace Chronology
