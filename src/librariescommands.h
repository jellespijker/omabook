#pragma once

#include <QStringList>

// The `libraries` command: the libraries this computer knows, and which is in
// use; moved out of cli.cpp, which had no room left, as `hub` came in (#179).
int runLibrariesCli(const QStringList &arguments);
