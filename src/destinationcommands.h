#pragma once

#include <QStringList>

// `omabook snapshot destination add|list|test|copy|remove` (#182): where else
// the snapshots are kept, a second disk or a place away from home, each its
// own restic repository with its own recovery key; on a hub with --hub.
int runSnapshotDestinationCli(const QStringList &arguments);
