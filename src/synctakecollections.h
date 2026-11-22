#pragma once

#include <QHash>
#include <QMap>
#include <QString>

#include "library.h"
#include "spacereducer.h"

// What each side of a sync between two Omabooks that speak the change log
// takes in of the collections, once both hold the same operations: each as
// both sides' operations merge by the log's rules (each field the latest; of
// the books in it by hand, an add over a concurrent remove). Each that the
// log says otherwise than here is written as it says, a parent before what is
// in it. One removed that this side never had is not made.
namespace SyncTakeCollections {

// `merged` is both sides' operations reduced, `books` this side's books by the
// id they stand for, `same` the id each book's id stands for
// (SpaceReducer::sameAs). Answers how many were written here; with `dryRun`,
// how many would be.
int take(Library &library, const QMap<QString, ReducedCollection> &merged, const QHash<QString, qint64> &books,
         const QHash<QString, QString> &same, bool dryRun);

} // namespace SyncTakeCollections
