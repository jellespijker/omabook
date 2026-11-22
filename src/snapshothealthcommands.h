#pragma once

#include <QDateTime>
#include <QStringList>
#include <expected>

#include "failure.h"
#include "snapshots.h"

// `omabook snapshot status` and `omabook snapshot check` (#184): how the
// snapshots stand, in each repository they are kept in, with the warnings
// and what to do about each; and restic's check of them, light or reading the
// data back. On a hub with --hub, its libraries' and its repositories'.
int runSnapshotHealthCli(const QStringList &arguments);

// After the nightly snapshot of `library` on this computer: how it went kept,
// a look at what each repository holds, and each new warning told on the
// desktop and on standard error.
void afterNightly(const Snapshots &snapshots, const Library &library, const std::expected<void, Failure> &outcome, const QDateTime &now);
