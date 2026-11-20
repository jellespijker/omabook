#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtTypes>
#include <expected>
#include <optional>

#include "spacereplay.h"
#include "spacesnapshot.h"

class Library;

// A published space this library subscribes to (ADR 0001, *Spaces*; #4):
// someone else's collection, replicated read-only and kept as a layer beside
// the library, never in its rows.
//
// Its operations are kept in the log (`ops`) under the space's own id, as the
// owner wrote them, and a published space's id ("published:…") keeps them out
// of what the library's rows are made of (`live_ops`), so nothing of it merges
// into the subscriber's books, collections or graph; they are read here, by
// the space (`space_ops`). A subscription travels to the subscriber's other
// computers beside the log, sealed to their person (SpaceCarry, #51), and
// whether they follow it is said in their personal space (follow). The space is
// recorded in `spaces` as published, its owner a writer by their person id
// and this computer a reader by its device id, which is what makes it a
// subscription rather than a space this library publishes.
namespace SubscribedSpace {

inline constexpr auto readerRole = "reader";

struct Subscription {
    QString space;         // "published:…"
    QString name;          // the space's name: the collection's, as its owner calls it
    QString owner;         // the owner's person id
    QString ownerName;     // as the owner chose to be called
    qint64 collection = 0; // the collection here that holds its books (link), 0 when none
};

// What of a snapshot is new here, and how much of it was had already.
struct Accepted {
    qsizetype added = 0;
    qsizetype known = 0;
};

// Whether the operations of `snapshot` may be taken in as a subscribed space:
// a published space's, with an owner whose id is a person's key, every
// operation of that space and written by that owner, and not a space this
// library publishes itself; a signed operation is checked as replication
// checks one (SignedOps::verify), an unsigned one taken in while published
// spaces go unsigned. The one place imported operations are checked before
// they are kept.
[[nodiscard]] std::expected<void, QString> verify(const Library &library, const SpaceSnapshot &snapshot);

[[nodiscard]] bool isSubscribed(const Library &library, const QString &space);

// Says in the personal space whether the person follows the space (#51), so
// that subscribing, and unsubscribing, on one of their computers does the
// same on the others: an operation (entity "subscription", the space's id,
// field "followed", "1" or "0") like any other of theirs, the latest winning.
void follow(const Library &library, const QString &space, bool followed);
// What the personal space last said of following the space; nothing when it
// never said (a subscription from before #51).
[[nodiscard]] std::optional<bool> followed(const Library &library, const QString &space);
[[nodiscard]] QList<Subscription> list(const Library &library);

// Every operation held of the space, the snapshot and those after it.
[[nodiscard]] QList<SpaceOperation> operations(const Library &library, const QString &space);
// The operations of `snapshot` this library doesn't hold yet: of each, the
// same device, clock, thing, field and value. So a newer snapshot of the same
// space brings only what is new, whatever its owner numbered them, and the
// same snapshot twice brings nothing.
[[nodiscard]] QList<SpaceOperation> lacking(const Library &library, const SpaceSnapshot &snapshot);

// Takes the snapshot in as a subscribed space, once verified: records the
// space and appends the operations it lacks, each under the owner's device
// and number, or the next number of that device here where the owner's
// numbering started again (a snapshot numbers its operations anew until
// publishing keeps them, #3).
[[nodiscard]] std::expected<Accepted, QString> accept(const Library &library, const SpaceSnapshot &snapshot);

// The collection here that stands for the space: a collection stands for one
// space at most, and a space for one collection.
void link(const Library &library, const QString &space, qint64 collection);
[[nodiscard]] qint64 linkedCollection(const Library &library, const QString &space);

// Where a book of the space (by its id there) is here: the library's own copy
// found when subscribing or the wishlist entry made for it, as recorded then,
// else the book here of the same id (the same file); 0 for none. What the
// layer's graph says of the book is cut at its reader's place in that one
// (GraphLayers, #5).
void placeBook(const Library &library, const QString &space, const QString &book, qint64 here);
[[nodiscard]] qint64 bookHere(const Library &library, const QString &space, const QString &book);
// The other way: the space's id of the book `here`, as placed; empty for none.
[[nodiscard]] QString bookIn(const Library &library, const QString &space, qint64 here);
// Of the books placed here for a space (also a shared one) that the space
// knows by a file (its id "b:" and the file's hash) and that have no file
// here, a wishlist entry: the hash and the book here. What adding that very
// file fills, rather than making the book twice (importBooks, #63).
[[nodiscard]] QHash<QString, qint64> wishedFiles(const Library &library);

// Drops the layer: the space, its operations (Compaction::forgetSubscribed),
// the link, where its books are here and where it was followed from
// (SpaceLinks). The collection it was linked to stays the library's own.
// How many operations left the log.
[[nodiscard]] std::expected<qint64, QString> drop(const Library &library, const QString &space);

} // namespace SubscribedSpace
