#pragma once

#include <QJsonValue>
#include <QString>
#include <cstdio>

// How the command line says its answers and why it failed.
namespace CliOutput {

// A line, flushed at once.
void print(FILE *stream, const QString &text);
// Says why on standard error; the exit code of a command that failed.
int fail(const QString &why);
void printJson(const QJsonValue &value);

} // namespace CliOutput
