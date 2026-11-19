#pragma once

#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QtTypes>

#include "library.h"
#include "remote.h"
#include "sync.h"

// What a sync still carries by the manifest only for an Omabook from before
// the change log, whose manifest says no "log": of a book both sides have,
// its details (the newer, field by field), what is said of its reading and
// its reading place, merged as they always were, and given to the other side
// by its commands. Between two Omabooks that speak the log they travel
// through it (synclog.h, synctake.h). This goes when the manifest sync is
// retired (ADR 0001, Migration 6).
namespace OlderPeer {

// One sync with an older Omabook: what a merge needs of it.
struct Sync {
    Library &library;           // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    const Remote &remote;       // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    QTemporaryDir &scratch;     // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    QString &folderThere;       // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): a folder there, made when first needed
    int &counter;               // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): numbers the files in `scratch`
    const SyncOptions &options; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    SyncSummary &summary;       // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    bool current = false;       // the Omabook there keeps when details changed, and takes them
};

// Of a book both sides have, here `mine` and in their manifest `theirs`
// (one that just `arrived` from there needs only its reading place): takes
// what is newer there, and gives them what is newer here.
void mergeBook(const Sync &sync, qint64 mine, bool arrived, const QJsonObject &theirs);

// Gives a book just sent to them, `theirId` there, its place here.
void givePlace(const Library &library, const Remote &remote, qint64 mine, qint64 theirId);

} // namespace OlderPeer
