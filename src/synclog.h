#pragma once

#include <QJsonArray>
#include <QJsonObject>

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
// their arrival wrote, and their places.
SyncTake::Taken replicate(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary,
                          const SyncOptions &options);

// There: takes in a request from replicate() and answers it.
[[nodiscard]] QJsonObject answer(Library &library, const QJsonObject &request);

} // namespace SyncLog
