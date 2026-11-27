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
    bool acceptRenames = false;                                     // a rename is done by its time (SyncRenames)
    bool dryRun = false;
    // On the computer that runs the sync, the other one and its manifest's
    // books: covers are pictures, which travel as files, so only the side
    // that reaches the other fetches theirs and gives its own.
    const Remote *remote = nullptr;
    QJsonArray books;
    // What the other side holds now: its files' hashes, and the entry keys
    // of its books, with a file or not. A book with one of them is still a
    // book there, as the one a merge kept is, or the copy kept of two that
    // were one book (#58), and is never removed here.
    QSet<QString> held;
};

// The names (files' hashes, every book's entry keys) of what a library holds.
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
// didn't change here since (as the manifest sync decided it), then takes
// their newer details and reading, the collections, the bookmarks and
// highlights (synctakeannotations.h) and what the general knowledge graph
// lacks (synctakegraph.h). Counts what changed here in
// `summary`.
[[nodiscard]] Taken take(Library &library, std::span<const SpaceOperation> incoming, const Peer &peer, SyncSummary &summary);

// Which of the other side's names elsewhere for a book this one takes: only
// within one book (#164). None of a book paired on less than its global id,
// which may be another's (#158); and never one `stray` names, that it has of
// a book kept apart from it (StrayIdentifiers). And the fields of its reading
// ("shelf", "rating", "finished") their device wrote later than this
// one's, of a book paired by its global id, but empty ones: each taken (ADR 0009, #213).
struct Names {
    bool take = true;
    QSet<QString> stray;
    QSet<QString> newerReading;
};

// Takes in what the other side says of a book's reading: each field `names`
// says is newer there, and where this side says nothing (an empty shelf, no
// stars, no finished day; a paper copy either side has is had); and the
// names elsewhere this one lacks, as `names` says.
// Whether that changes anything; with `dryRun` only that. What it changes
// goes into `plan`, when there is one (the sync guard's, #187).
bool fillReading(Library &library, qint64 id, const QJsonObject &theirs, bool dryRun, const Names &names = {}, SyncPlan *plan = nullptr);

// `omabook set` arguments that give the other side a book's reading and,
// with `names`, its names elsewhere, with `detailsAt` (for an Omabook there
// that keeps it) the time its details are stamped with there; and with
// `readingAt` (for one that takes it, "reading_at" in its manifest) the time
// its reading is, so it keeps the time it had where it was changed (#243).
[[nodiscard]] QStringList readingArguments(qint64 id, const Book &book, std::optional<qint64> detailsAt, bool names = true,
                                           bool readingAt = false);

// A book fetched from there, added here with the id it has there and the hash its first file came in with,
// in one batch, so the log has it by that id from the start (#255).
[[nodiscard]] Library::Added addAsThere(Library &library, const QString &file, const BookMeta &meta, const QJsonObject &theirs);
// What `add` there is given to keep a book's id here and the hash its file came in with, for an Omabook that
// keeps them ("keeps_id" in its manifest, #255).
[[nodiscard]] QStringList keptIdArguments(const Library &library, qint64 id, const QList<FormatFile> &files, const QJsonObject &manifest);

// Whose details settle the undecided ones, as the other side sees it.
[[nodiscard]] QString otherSide(SyncOptions::Details details);
[[nodiscard]] SyncOptions::Details detailsFrom(const QString &name);

} // namespace SyncTake
