#pragma once

#include <QString>
#include <QVariantMap>
#include <QtTypes>
#include <functional>

#include "chronologychart.h"
#include "chronologyread.h"
#include "eventtime.h"
#include "graph.h"

class Library;

// How the chronology's window (#44) says what is known of an event: its
// card, with its time in words, the lanes it is on, what it involves, the
// contradictions it is in with what was said where, and where to go back
// to on the page. One place for these words, so the card, the ruler and the
// list of what has no time say them the same way.
namespace ChronologyCard {

// A moment as the reader would say it: "6 June 1944", "June 1944", "44 BC"
// on the real calendar, as the book writes it on a story's own ("3019-03-25").
[[nodiscard]] QString point(const QString &calendar, const EventTime::Point &point);
// A time as the reader would say it: a moment, a span ("1939 – 1945"),
// either with "around", and the calendar named when it is a story's own.
[[nodiscard]] QString when(const EventTime &time);
// Where in a book something was revealed: {bookId, section, fraction,
// anchor, title, readable}, or empty for what no book says.
[[nodiscard]] QVariantMap where(const Library &library, const GraphMoment &moment);

// What the card shows of an event: {id, name, summary, from, when, said,
// timelines, during, involved, books, contradictions, page, graph}.
// `laneName` names a lane of the chart.
[[nodiscard]] QVariantMap of(const Library &library, const Chronology::Reading::Read &read, const Chronology::Chart::Chart &chart,
                             qint64 event, const std::function<QString(int)> &laneName);

} // namespace ChronologyCard
