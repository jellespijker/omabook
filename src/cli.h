#pragma once

#include <QString>
#include <QStringList>

// The commands that need no window: what agents and scripts use. Each takes
// --json, prints errors to standard error, and exits 0 or 1.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);
