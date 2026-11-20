#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QtTypes>

#include "chronologychart.h"
#include "graph.h"
#include "library.h"

// What the window's chronology (#44) shows, read from the library: the
// chronicle of each part of the library it is opened in (Chronology::Tenant,
// which is the side of the book, series or collection it was opened from,
// or every side of a whole graph, each apart), with the graphs of the
// spaces subscribed to laid in as the reader knows them (GraphLayers, #5),
// and of each event what the filters ask: the books it was met in, and
// the characters, places, groups and themes it involves.
//
// Everything is cut where the reader is (GraphContext::readerKnows and
// GraphContext::layers): no event, time, order or involvement from past it.
namespace Chronology::Reading {

// Where the chronology is opened from.
struct From {
    enum class What { Scope, Book, Series, Collection };
    What what = What::Scope;
    QString scope; // the graph's; "" for the general one
    qint64 bookId = 0;
    QString name; // the series' or the collection's
    // The tenant to read in, by its global id, as the window chose it (#96);
    // empty for that of what it was opened from, or each of a whole graph's.
    QString tenant;
};

// What a filter can ask of an event.
struct Event {
    GraphNode node;
    QList<GraphPlace> met;  // where it was first met in each book, as far as the reader read
    QSet<qint64> books;     // the books it was met in; none for one only implied
    QList<qint64> involved; // characters, places, groups and themes, by number
    QString from;           // whose shared graph it is of; empty for one's own
};

struct Read {
    QList<Chart::Part> parts;
    QHash<qint64, Event> events;
    // The things events involve, by number, as the reader knows them.
    QHash<qint64, GraphNode> involved;
    // The books of what it was opened from (a book, a series' or a
    // collection's); empty for a whole graph.
    QSet<qint64> books;
};

// The kinds of thing an event can involve that the chronology filters by.
[[nodiscard]] const QStringList &involvedKinds();

[[nodiscard]] Read read(const Library &library, const From &from);

} // namespace Chronology::Reading
