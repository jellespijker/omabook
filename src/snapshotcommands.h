#pragma once

#include <QStringList>
#include <optional>

#include "snapshots.h"

// The `snapshot` command (#181): set snapshots up, take one now, list them,
// and the nightly one with its thinning, for a timer or the hub's daemon.
int runSnapshotCli(const QStringList &arguments);

// The moment a command is about to change much at once (a sync, an import,
// a tidy that changes something), as runCli is given it; none for the rest,
// and for a look that changes nothing (--dry-run, --status, a tidy's list).
[[nodiscard]] std::optional<SnapshotReason> riskyMoment(const QStringList &arguments);

// A moment snapshot of the command's library before a risky command, when
// snapshots are set up here and the library changed since its last one. One
// that can't be taken is said on standard error, and the command goes on.
void snapshotBeforeRisky(const QStringList &arguments);
