#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>

#include "graph.h"
#include "graphpassages.h"
#include "library.h"

// What a model's answer says of when its events happen (#42), laid into the
// chronology (chronology.h) as the graph is built: an event's "when", the
// events it comes before or after, and the timeline it is on. Each is
// checked as the chronology checks it, and what isn't one is dropped and
// counted, never kept. Each is revealed where the text says it, within the
// passages the answer came from, so the reader's cut holds.
namespace GraphBuild {

struct Timed {
    int times = 0;   // "when" facts kept
    int orders = 0;  // relations of order kept
    int placed = 0;  // events put on a timeline
    int dropped = 0; // times and orders given that were none, or of no event

    Timed &operator+=(const Timed &other);
    [[nodiscard]] QJsonObject toJson() const;
};

// A thing of the graph by the name a model gave it; 0 when there is none.
using NodeOf = std::function<qint64(const QString &name)>;

// Whether an edge's relation is one of order, which takes its own way in.
[[nodiscard]] bool ofOrder(const QString &relation);

// Lays in what `answer` says of the times of its events: the "when" and
// "timeline" of its nodes and its edges of order, placed in `passages`.
// Every event the answer gives goes on a timeline: the book's own unless
// the model names another, and real history too when it is dated on the
// real calendar.
[[nodiscard]] Timed applyTimes(Graph &graph, const Library &library, qint64 bookId, const QList<Passage> &passages,
                               const QJsonObject &answer, const NodeOf &nodeOf);

} // namespace GraphBuild
