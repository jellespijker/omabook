#include "compaction.h"

#include <QSqlQuery>
#include <QVariant>

#include "changelog.h"
#include "library.h"

namespace {

// The fields of which every operation counts, not only the latest: what was
// met where, what was learned, how things relate, the days read, and the
// links that line up ids (SpaceReducer's growing sets).
constexpr auto growing = "(field LIKE 'seen:%' OR field IN ('revealed', 'link', 'range', 'same_as'))";

[[nodiscard]] qint64 count(const Library &library, const char *sql) {
    QSqlQuery query = ChangeLog::query(library, QString::fromLatin1(sql));
    return query.next() ? query.value(0).toLongLong() : 0;
}

} // namespace

Compaction::Result Compaction::compact(const Library &library) {
    const Library::Batch batch(library);
    // Up to now, per space and device; the snapshot is made again of the
    // snapshot so far and what came after it, which is all that matters of `ops`.
    ChangeLog::exec(library, "CREATE TEMP TABLE IF NOT EXISTS compacting AS SELECT * FROM live_ops WHERE 0");
    ChangeLog::exec(library, "DELETE FROM temp.compacting");
    ChangeLog::exec(library, "INSERT INTO temp.compacting SELECT * FROM live_ops");
    ChangeLog::exec(library, "CREATE INDEX IF NOT EXISTS temp.compacting_field ON compacting (device, entity, id, field, hlc)");
    ChangeLog::exec(library, "DELETE FROM snapshot_ops");
    ChangeLog::exec(library, QString("INSERT INTO snapshot_ops SELECT * FROM temp.compacting AS c WHERE %1 OR NOT EXISTS ("
                                     "SELECT 1 FROM temp.compacting AS later WHERE later.device = c.device AND later.entity = c.entity "
                                     "AND later.id = c.id AND later.field = c.field AND later.hlc > c.hlc)")
                                 .arg(QString::fromLatin1(growing).replace("field", "c.field")));
    ChangeLog::exec(library, "INSERT OR REPLACE INTO snapshot_marks (space, device, seq) SELECT space, device, MAX(seq) FROM ops "
                             "GROUP BY space, device");
    ChangeLog::exec(library, "DROP TABLE temp.compacting");
    return {.operations = count(library, "SELECT COUNT(*) FROM ops"), .live = count(library, "SELECT COUNT(*) FROM live_ops")};
}

void Compaction::compactWhenWorthIt(const Library &library) {
    const qint64 after = count(library, "SELECT COUNT(*) FROM ops AS o LEFT JOIN snapshot_marks AS m ON m.space = o.space "
                                        "AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0)");
    if (after > 1000 && after > count(library, "SELECT COUNT(*) FROM snapshot_ops"))
        compact(library);
}
