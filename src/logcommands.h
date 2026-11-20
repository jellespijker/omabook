#pragma once

#include <QStringList>

// The `logs` command: what Omabook did and what went wrong, read from the one
// log the window, every command and the MCP server write to.
int runLogsCli(const QStringList &arguments);
