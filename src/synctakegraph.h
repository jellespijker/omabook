#pragma once

#include <QHash>
#include <QSet>
#include <QString>

#include "library.h"
#include "spacereducer.h"

// What each side of a sync between two Omabooks that speak the change log
// takes in of the general knowledge graph (the personal space's), once both
// hold the same operations: the things this side lacks, where they were met,
// what was learned of them and how they relate, and the days read, each at
// the moment it was revealed, the earlier one standing. Like the graph sync
// before it (graphsync.h), it only adds: what the other side holds and this
// one doesn't. A thing this side knows by another name is found once with the
// graph sync's rules (GraphMatch::matchNodes) and linked by a same-as
// operation; from then on the log folds the two ids into one, and it goes by
// the names it has on either side.
//
// How far a book's graph was built whole elsewhere (by a hub, GraphBuilt) is
// taken for how far it is built here, where that is further (#219).
//
// The things a library makes of its own books (a book, its authors, series
// and tags) are made on each side from the library, as before; relations to
// them find them here by what they stand for.
namespace SyncTakeGraph {

// `merged` is both sides' operations reduced, `books` this side's books by the
// id they stand for, `same` the ids of things and the one each stands for
// (SpaceReducer::sameAs). Answers how many things, facts, places, relations
// and reading days were new here.
struct Things {
    QSet<QString> theirs;          // the ids of the things the other side's device wrote of
    QHash<QString, QString> names; // the name each thing's id was given
};
int take(Library &library, const ReducedState &merged, const QHash<QString, qint64> &books, const QHash<QString, QString> &same,
         const Things &things);

} // namespace SyncTakeGraph
