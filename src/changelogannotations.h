#pragma once

#include <QList>
#include <QtTypes>

class Library;

// Takes a book's bookmarks and highlights (the `annotations` table, every
// kind of it: one entity, "annotation", for all of them, #66) into the
// personal space's log, by a global id each (Spaces::annotationId). Each
// field (its book's id, kind, section, fraction, words, note, when it was
// made) is a field of the log, the latest winning; one removed is a
// tombstone, unless its book went with it: a book removed here and kept
// elsewhere keeps its bookmarks there.
namespace ChangeLogAnnotations {

// Every one of them: the baseline's, or on opening what bypassed the log.
void syncToLog(const Library &library, bool baseline);
// The rows a write touched, by their local ids, and a tombstone for each that is gone.
void syncRowsToLog(const Library &library, const QList<qint64> &ids);
// On opening: a tombstone for each this computer had and has no more.
void detectDeleted(const Library &library);

// Whether the log took in every write to them, by their own counter; and
// recording that it did (ChangeLog::markLogged).
[[nodiscard]] bool counted(const Library &library);
void markLogged(const Library &library);

} // namespace ChangeLogAnnotations
