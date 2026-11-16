#pragma once

#include <QStringList>

// The `sync` and `manifest` commands: keeping two computers' libraries in step.
int runSyncCli(const QStringList &arguments);
int runManifestCli(const QStringList &arguments);
