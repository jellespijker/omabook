#pragma once

#include <QString>
#include <expected>

#include "failure.h"

class Library;

// One book put back as an earlier copy of its library had it (#183): its
// details, its annotations, or both, from a snapshot's database (SnapshotCopy)
// into the live library, the book found by its global id (ADR 0009). What
// writes it is the library's one writer: the command on a computer, the
// keeper on a hub (HubSnapshots), after a moment snapshot that undoes it.
//
// Nothing of the live library is lost on the way: details are set to the
// earlier ones (and stamped now, so a sync carries them on); an annotation
// removed since comes back with its own global id, one changed since goes back
// to how it was, and one made since stays. An ink page gets back the strokes
// it had; strokes drawn since stay.
namespace BookRestore {

struct Parts {
    bool details = false, annotations = false;
};

struct Restored {
    bool details = false; // whether any detail changed
    int annotations = 0;  // annotations put back: come back, or changed back
    int strokes = 0;      // ink strokes put back
};

// The book of `globalId` in `live` put back as `from` has it. Refused when
// `from` has no such book (SnapshotBookNotFound), or the live library has
// it no longer (RestoreBookGone: a whole restore brings that back).
[[nodiscard]] std::expected<Restored, Failure> restore(Library &live, const Library &from, const QString &globalId, Parts parts);

} // namespace BookRestore
