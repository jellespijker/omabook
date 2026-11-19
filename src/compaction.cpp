#include "compaction.h"

#include <QSqlQuery>
#include <QVariant>

#include "changelog.h"
#include "library.h"
#include "spaces.h"

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
    // A mark only moves on: what is left in `ops` of a device may all lie
    // before its mark now, kept for a member that hasn't seen it, and the
    // snapshot already holds what matters of it.
    ChangeLog::exec(library, "INSERT INTO snapshot_marks (space, device, seq) SELECT space, device, MAX(seq) FROM ops "
                             "WHERE 1 GROUP BY space, device ON CONFLICT(space, device) DO UPDATE SET seq = MAX(seq, excluded.seq)");
    ChangeLog::exec(library, "DROP TABLE temp.compacting");
    // What the snapshot covers, of what every other member of its space has
    // seen (a space with no other member is this computer's): the snapshot
    // takes the place of what it keeps of it, and what it leaves out is no
    // longer needed anywhere. What a member hasn't seen stays until it has.
    const qint64 before = count(library, "SELECT COUNT(*) FROM ops");
    ChangeLog::exec(
        library,
        "DELETE FROM ops WHERE seq <= COALESCE((SELECT m.seq FROM snapshot_marks AS m WHERE m.space = ops.space "
        "AND m.device = ops.device), 0) "
        "AND NOT EXISTS (SELECT 1 FROM space_members AS p JOIN log_ids AS named ON named.name = p.space "
        "WHERE named.id = ops.space AND p.person <> ? AND COALESCE((SELECT v.seq FROM member_vectors AS v "
        "WHERE v.space = ops.space AND v.member = p.person AND v.device = ops.device), 0) < ops.seq)",
        {Spaces::deviceId(library)});
    const qint64 left = count(library, "SELECT COUNT(*) FROM ops");
    // The pages freed go back to the file only where the library vacuums incrementally.
    if (count(library, "PRAGMA auto_vacuum") == 2)
        ChangeLog::exec(library, "PRAGMA incremental_vacuum");
    return {.operations = left, .live = count(library, "SELECT COUNT(*) FROM live_ops"), .dropped = before - left};
}

void Compaction::compactWhenWorthIt(const Library &library) {
    const qint64 after = count(library, "SELECT COUNT(*) FROM ops AS o LEFT JOIN snapshot_marks AS m ON m.space = o.space "
                                        "AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0)");
    if (after > 1000 && after > count(library, "SELECT COUNT(*) FROM snapshot_ops"))
        compact(library);
}

void Compaction::rememberSeen(const Library &library, const QString &member, const Replication::Vector &held) {
    if (member.isEmpty())
        return;
    const Library::Batch batch(library);
    for (auto space = held.cbegin(); space != held.cend(); ++space)
        for (auto device = space->cbegin(); device != space->cend(); ++device)
            ChangeLog::exec(
                library,
                "INSERT INTO member_vectors (space, member, device, seq) VALUES (?, ?, ?, ?) "
                "ON CONFLICT(space, member, device) DO UPDATE SET seq = MAX(seq, excluded.seq)",
                {ChangeLog::internId(library, space.key()), member, ChangeLog::internId(library, device.key()), device.value()});
}
