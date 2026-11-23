#pragma once

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

#include "library.h"
#include "spacereducer.h"

// What each side of a sync between two Omabooks that speak the change log
// takes in of the bookmarks and highlights (#66), once both hold the same
// operations: each as the log's rules merge both sides' (of each field the
// latest), made, changed or removed here to match, on the book here it
// stands for. One of a book this side doesn't have waits for the book: run
// again once books have moved (SyncLog::finish), it is made then.
//
// What is written here goes into the log as this computer's own, with the
// stamps it came with (ChangeLogFields::write), so taking it again changes
// nothing.
namespace SyncTakeAnnotations {

// `merged` is both sides' operations reduced, `books` this side's books by the
// id they stand for, `same` the id each book's id stands for
// (SpaceReducer::sameAs). Answers how many were made, changed or removed
// here; with `dryRun`, how many would be. The titles of the books one would
// move to from another book go into `moved`, for the sync guard (#187).
int take(Library &library, const QMap<QString, ReducedAnnotation> &merged, const QHash<QString, qint64> &books,
         const QHash<QString, QString> &same, bool dryRun, QStringList &moved);

} // namespace SyncTakeAnnotations
