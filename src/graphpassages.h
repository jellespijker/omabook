#pragma once

#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

#include "document.h"
#include "graph.h"

// The text a graph is built from, as passages, and where in them the words a
// model gives point: the place a fact was revealed at, never after the end of
// what was sent, so the graph is never ahead of its reader (graphbuilder.h).
namespace GraphBuild {

// A stretch of one section's text, as it is sent to a model: from where to
// where in it (0 to 1). Not an annotation's ::Passage (bookplace.h): all of
// its text, and never found again.
struct Passage {
    int section = 0;
    double from = 0, to = 1;
    QString text;
};

// The most of one answer that is read, whatever the model sends.
constexpr int mostNodes = 40, mostEdges = 60;

// The text of a book after one place (the very start when there is none)
// and up to another, a passage per section. In a paged book a page has been
// read once the reader is on it, and its facts are placed at its start.
QList<Passage> passages(const Document &document, const std::optional<GraphPlace> &after, const GraphPlace &upTo);
qsizetype size(const QList<Passage> &passages);

// A section number as a model writes it: a number, or a number in a string;
// -1 when it is neither.
[[nodiscard]] int sectionOf(const QJsonValue &value);
// Where in the passages some words first occur, of the section the model
// named (all of them when it named none that is there): the place, and the
// words as the text has them, to find it by.
[[nodiscard]] std::optional<GraphMoment> momentOf(qint64 bookId, const QList<Passage> &passages, int section, const QStringList &phrases);
// Where in the passages a thing first shows: where one of its names first
// occurs in the section the model named, and where none does, the end of
// that section's part.
[[nodiscard]] GraphMoment placeOf(qint64 bookId, const QList<Passage> &passages, int section, const QStringList &names);
// Where a fact was revealed: at the words the model quoted from the text,
// when the text has them; else `fallback`.
[[nodiscard]] GraphMoment revealedAt(qint64 bookId, const QList<Passage> &passages, int section, const QJsonValue &quote,
                                     const GraphMoment &fallback);

} // namespace GraphBuild
