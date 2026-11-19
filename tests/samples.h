#pragma once

// The library samples of every release (tests/data/library-<version>.sql),
// and a library's travelling state as one JSON object to compare: for the
// tests of the change log and of syncing through it (ADR 0001).

#include <QJsonObject>
#include <QString>

class Library;

namespace Samples {

// Loads a sample's SQL dump into a new database at `dbPath`, and says its format.
void loadSqlDump(const QString &dumpPath, const QString &dbPath, int &formatOut);
// Everything of a library that travels, by global id, in a stable order.
QJsonObject librarySnapshot(const Library &library);

} // namespace Samples
