#pragma once

#include <QStringList>

// `omabook snapshot diff` and `omabook snapshot restore` (#183): the library
// diff between two snapshots, or one and now; and going back, the whole
// library into a new folder, or one book's details or annotations into the
// library after showing what changes and asking. runSnapshotCli hands them here.
int runSnapshotRestoreCli(const QStringList &arguments);
