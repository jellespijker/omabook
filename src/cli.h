#pragma once

#include <QString>
#include <QStringList>

// The commands that need no window: what agents and scripts use. Each takes
// --json, prints errors to standard error, and exits 0 or 1.
bool isCliCommand(const QString &word);
// The options every command takes (--library, --json, --model, --help) moved
// from before the command's name to just after it, so `omabook --library X
// list` is `omabook list --library X`. Arguments that don't start with
// options followed by a command (or `open`) come back as they are.
QStringList commandFirst(const QStringList &arguments);
QString cliSummary();
int runCli(const QStringList &arguments);
