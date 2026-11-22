#pragma once

#include <QList>
#include <QSet>
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

// The global ids of the whole graph, of every tenant, as the log names its
// rows: its things, relations and facts (what the library diff counts, #183).
// A row the log hasn't named yet is named as Spaces would name it.
struct GlobalIds {
    QSet<QString> things, relations, facts;
};
[[nodiscard]] GlobalIds globalIds(const Library &library);

} // namespace ChangeLogGraph
