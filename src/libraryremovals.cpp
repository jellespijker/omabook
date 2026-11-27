// What the library remembers of the books removed from it: by their files'
// hashes, and a book without a file by its entry keys, so that a sync never
// fetches one back, until it is added again.

#include <QSqlQuery>
#include <QVariant>

#include "library.h"

std::optional<qint64> Library::removedAt(const QString &hash) const {
    QSqlQuery found = query("SELECT removed_at FROM removed WHERE hash = ?", {hash});
    return found.next() ? std::optional<qint64>(found.value(0).toLongLong()) : std::nullopt;
}

QList<Library::Removal> Library::removals() const {
    QList<Removal> removals;
    for (QSqlQuery found = query("SELECT hash, removed_at FROM removed ORDER BY hash"); found.next();)
        removals.append({.key = found.value(0).toString(), .at = found.value(1).toLongLong()});
    return removals;
}

QStringList Library::removedHashes() const {
    QStringList hashes;
    for (QSqlQuery found = query("SELECT hash FROM removed"); found.next();)
        hashes << found.value(0).toString();
    return hashes;
}

bool Library::wasRemoved(const QString &hash) const { return query("SELECT 1 FROM removed WHERE hash = ?", {hash}).next(); }