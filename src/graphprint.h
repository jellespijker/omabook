#pragma once

#include <QJsonObject>
#include <QString>
#include <cstdio>

// How `omabook graph` shows its answers on a terminal, when not asked for JSON.
namespace GraphPrint {

// A line, flushed at once so a long command shows its progress.
void print(FILE *stream, const QString &text);
// A thing as one line: its number, kind, name and what is said of it.
QString nodeLine(const QJsonObject &node);
// What a book connects to elsewhere, a line a tie: both ends by name, and
// the book the far end is in.
void printBeyond(const QJsonObject &answer);
// A view as lines: the things, then how they relate, by name; then each
// graph someone shared ("layers"), apart.
void printView(const QJsonObject &view);

} // namespace GraphPrint
