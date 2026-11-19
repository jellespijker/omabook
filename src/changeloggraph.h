#pragma once

#include <QList>
#include <QString>
#include <QtTypes>

class Library;

// Synchronizes knowledge-graph entities (`graph_nodes`, `graph_facts`,
// `graph_edges`, `graph_seen`, `reading_log`) and their deletions with `ops`.
namespace ChangeLogGraph {

void syncToLog(const Library &library, bool baseline);
void syncNodeAt(const Library &library, qint64 nodeId, qint64 hlc);
void detectDeletedEntity(const Library &library, const QString &entity, const QString &table);

// The rows a write touched, by their local ids (see ChangeLog::afterWrite).
struct Rows {
    QList<qint64> nodes;
    QList<qint64> facts;
    QList<qint64> edges;
    QList<qint64> readingBooks; // books whose reading log changed
};
// Takes in just those rows, and a tombstone for each that is gone.
void syncRowsToLog(const Library &library, const Rows &rows);

} // namespace ChangeLogGraph
