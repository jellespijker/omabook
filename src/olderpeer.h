#pragma once

#include <QJsonObject>
#include <QtTypes>

#include "library.h"
#include "remote.h"
#include "sync.h"

// What a sync still carries by the manifest only for an Omabook from before
// the change log, whose manifest says no "log": the reading places, merged as
// they always were (the newer place wins, either way). Between two Omabooks
// that speak the log they travel through it (synclog.h). This goes when the
// manifest sync is retired (ADR 0001, Migration 6).
namespace OlderPeer {

// Of a book both sides have, here `mine` and in their manifest `theirs`,
// takes their place when it is newer, or gives them this side's.
void mergePlace(Library &library, const Remote &remote, qint64 mine, const QJsonObject &theirs, SyncSummary &summary, bool dryRun);

// Gives a book just sent to them, `theirId` there, its place here.
void givePlace(const Library &library, const Remote &remote, qint64 mine, qint64 theirId);

} // namespace OlderPeer
