#pragma once

#include <QJsonObject>
#include <QString>
#include <expected>
#include <optional>

#include "eventtime.h"
#include "graph.h"
#include "library.h"

// What `omabook chronology` and the chronology MCP tools do (#45), written
// once and answering in JSON: the chronology of the books read, cut where
// the reader is, and what is put into it by hand.
namespace ChronologyOps {

// Which chronology, and how much of it.
struct Ask {
    // Of whose events: one book's, a series', a collection's (and, where the
    // library's sides are kept apart, of that collection's side), or of every
    // book when none is named.
    qint64 book = 0;
    QString series, collection;
    // The graph it is read from: the general one when empty.
    QString scope;
    // Only one timeline, by its number or name.
    QString timeline;
    // Only what happens from one time to another (either may be left out),
    // on this calendar; on each timeline's own when none is named.
    QString from, to, calendar;
    // Where the reader is: as far as each book was read, unless the book is
    // cut at an earlier or later place (`upTo`, of `book`), or `whole` takes
    // everything that is known, read or not.
    std::optional<GraphPlace> upTo;
    bool whole = false;
};

// {"cut", "events": {"shown", "total"}, "books": {id: title}, "parts": [...]}:
// a part for each side of the library (one where it isn't split), each a
// chronicle ({"side", "lanes", "axes", "unplaced", "contradictions"}, see
// chronicle.h). "total" counts the events the reader knows of the books
// asked for, "shown" those left after the timeline and the stretch of time.
// What was put in by hand is marked "by_hand".
[[nodiscard]] std::expected<QJsonObject, QString> read(const Library &library, const Ask &ask);

// A time as it is said on the command line: "1944-06-06", a span
// "1939-09-01..1945-09-02" (either end may be left out), or a whole time as
// JSON (eventtime.h); on a calendar ("gregorian" when not named), roughly,
// and with the book's own words. Only words, with no time, is a time too.
struct Said {
    QString time, calendar, words;
    bool approximate = false;
};
[[nodiscard]] std::expected<EventTime, QString> timeOf(const Said &said);

// Writing it into the graph by hand, each checked as the graph's own
// writers check it (chronology.h), and revealed in no book: what is put in
// by hand wins over what the books say (chronologyorder.h). An event or a
// timeline is named by its number or its name.
[[nodiscard]] std::expected<QJsonObject, QString> setWhen(Graph &graph, const QString &event, const Said &said, const QString &scope = {});
// "before", "after", "during", "same time as" (or "same").
[[nodiscard]] std::expected<QJsonObject, QString> order(Graph &graph, const QString &event, const QString &relation, const QString &other,
                                                        const QString &scope = {});
[[nodiscard]] std::expected<QJsonObject, QString> place(Graph &graph, const QString &event, const QString &timeline,
                                                        const QString &scope = {});
// A timeline: whose it is (book, series, world, history or reader; the
// reader's when not said).
[[nodiscard]] std::expected<QJsonObject, QString> addTimeline(Graph &graph, const QString &name, const QString &kind = {});

} // namespace ChronologyOps
