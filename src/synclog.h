#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtTypes>

#include "library.h"
#include "remote.h"
#include "sync.h"
#include "synctake.h"

// The part of a sync that goes through the change log, between two Omabooks
// that both speak it (ADR 0001, Migration 3 and 4). So far that is the books'
// reading places, details and reading; the manifest carries the rest and
// leaves those out of it. With an Omabook from before the log, the manifest
// carries them as it always did (olderpeer.h).
//
// Here asks there for its manifest, which says the space, device and version
// vector of its log; here then hands over in one request the operations
// there lacks, its own vector, and the books there may know by another id,
// and there answers with the operations here lacks.
namespace SyncLog {

// What a manifest says of the log: this library's personal space, device and vector.
[[nodiscard]] QJsonObject introduction(const Library &library);

// Here: pairs with the computer whose `manifest` this is, sends it what it
// lacks and takes in what this library lacks; then each side takes from the
// log what is newer on the other (synctake.h), counted either way in
// `summary`. Answers the manifest's books, each with what the log says the
// other side holds of it, for the books this side lacks. A dry run changes
// nothing on either side. Run again once books have moved, it carries what
// their arrival wrote, and their places (finish()).
//
// With `carry`, it also carries the shared spaces and subscriptions one is in
// to the other side, and takes in what it carries back, when it is another
// of one's own computers (SpaceCarry, #51).
struct Replicated {
    // The manifest as it is now: its books each with what the log says the
    // other side holds of it, for the books this side lacks; the vector both
    // hold; and who the other side is once paired (SpaceCarry::introduction).
    QJsonObject manifest;
    QSet<qint64> gone; // the books here they removed (SyncTake::Taken)
};
Replicated replicate(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary, const SyncOptions &options,
                     bool carry = false);

// Here, at the end of a sync whose log went first (replicate()), with the
// manifest it answered: once more when books moved, for what their arrival
// wrote, and when there is anything to carry to the other side, which is
// another of one's own computers, once the books are where they go, so what
// is filed on arrival finds them (SpaceCarry::worthCarrying).
void finish(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary, const SyncOptions &options);

// There: takes in a request from replicate() and answers it.
[[nodiscard]] QJsonObject answer(Library &library, const QJsonObject &request);

} // namespace SyncLog
