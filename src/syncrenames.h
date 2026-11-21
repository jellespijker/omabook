#pragma once

#include <QJsonObject>
#include <QString>

#include "library.h"
#include "sync.h"
#include "syncmanifest.h"

// A book's title, authors and ISBN say which book it is: a sync never changes
// them by itself for a book paired on less than its global id (an identifier,
// BookLinks::weaklyPaired), where two different books may have been taken for
// one (#158, ADR 0009). Such a change is a rename: listed by a dry run, and
// done only with consent, `--accept-renames` (by the details' times, as the
// rest of them go) or `--details theirs` / `--details mine` (theirs or this
// side's, whatever the times). Until then the book takes none of the other's
// details.
namespace SyncRenames {

// What consent there is: `--details`, and `--accept-renames`.
struct Consent {
    SyncOptions::Details details = SyncOptions::Details::Undecided;
    bool accept = false;
};

// Of `merged`, the details a take decided for `here` from `theirs`, keeps
// the title, authors and ISBN as `consent` says, and counts the rename there
// is in `summary`, as "<title>: title → …; authors … → …", among the ones
// left for want of consent when it is.
void hold(const Book &here, const QJsonObject &theirs, Consent consent, SyncManifest::MergedBookDetails &merged, SyncSummary &summary);

} // namespace SyncRenames
