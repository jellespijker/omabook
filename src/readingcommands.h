#pragma once

#include <QString>
#include <QStringList>

// The commands of one's reading: `progress`, how far a book has been read,
// and `stats` (#68), the reading statistics of a year, of the whole library
// or one collection, one book's reading, and the year's goal.
int runProgressCli(const QStringList &arguments);
int runStatsCli(const QStringList &arguments);

// A time as the command line says it: "3 h 05 min", "12 min", "0 min".
[[nodiscard]] QString hoursAndMinutes(qint64 seconds);
