#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <optional>
#include <span>

#include "library.h"
#include "remote.h"
#include "spacereplay.h"
#include "sync.h"

// What each side of a sync between two Omabooks that speak the change log
// takes in from it, once both hold the same operations (ADR 0001, Migration 4).
//
// Nothing is given to the other side by command: each side looks at what the
// log says the other computer holds (the operations its device wrote, which is
// what its rows are) and takes what is newer there, by the same rules on both
// sides, so both end alike. A book's details go field by field by each
// field's time, and where neither side knows a time (details from before
// times were kept) they stay as they are, undecided, until `--details mine`
// or `--details theirs` settles them. What is said of its reading is filled in
// where this side says nothing.
namespace SyncTake {

// Who the other side is, and how far to go.
struct Peer {
    QString device;                                                 // the other computer's device id in the log
    SyncOptions::Details details = SyncOptions::Details::Undecided; // whose undecided details win
    bool dryRun = false;
    // On the computer that runs the sync, the other one and its manifest's
    // books: covers are pictures, which travel as files, so only the side
    // that reaches the other fetches theirs and gives its own.
    const Remote *remote = nullptr;
    QJsonArray books;
    // What the other side holds now: its files' hashes, and the entry keys
    // of its books without a file. A book with one of them is still a book
    // there, as the one a merge kept is, and is never removed here.
    QSet<QString> held;
};

// The names (files' hashes, entry keys of books without a file) of what a library holds.
[[nodiscard]] QSet<QString> heldBy(const Library &library);
// The same of a manifest's books.
[[nodiscard]] QSet<QString> heldBy(const QJsonArray &books);

// What a take leaves for the rest of a sync to do.
struct Taken {
    // The other side's books of `Peer::books`, each with what the log says
    // it holds of it (its details and reading), as a manifest would have said.
    QJsonArray books;
    // The books here the other side removed: removed here too (but on a dry
    // run), or kept because they changed here since; never given back.
    QSet<qint64> gone;
};

// Takes in what the log says of the books both sides hold, `incoming` being
// the operations just come from the other side (already stored, but on a dry
// run): removes the ones the other side removed, all of each, and that
// didn't change here since (as the manifest sync decided it), then takes their newer details and reading and the collections.
// Counts what changed here in `summary`.
[[nodiscard]] Taken take(Library &library, std::span<const SpaceOperation> incoming, const Peer &peer, SyncSummary &summary);

// Takes in what the other side says of a book's reading where this side says
// nothing: an empty shelf, no stars, no finished day; a paper copy either side
// has is had; the names elsewhere this one lacks. Whether that changes
// anything; with `dryRun` only that.
bool fillReading(Library &library, qint64 id, const QJsonObject &theirs, bool dryRun);

// `omabook set` arguments that give the other side a book's reading and its
// names elsewhere, with `detailsAt` (for an Omabook there that keeps it) the
// time its details are stamped with there.
[[nodiscard]] QStringList readingArguments(qint64 id, const Book &book, std::optional<qint64> detailsAt);

// Whose details settle the undecided ones, as the other side sees it.
[[nodiscard]] QString otherSide(SyncOptions::Details details);
[[nodiscard]] SyncOptions::Details detailsFrom(const QString &name);

} // namespace SyncTake
