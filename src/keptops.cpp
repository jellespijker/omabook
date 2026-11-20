#include "keptops.h"

#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

#include "changelog.h"
#include "log.h"
#include "opkinds.h"
#include "statements.h"

using namespace Qt::StringLiterals;

namespace {

KeptOps::Taker &taker() noexcept {
    static KeptOps::Taker given = nullptr;
    return given;
}

// One kept, with its space and device as `ops` numbers them.
struct Held {
    SpaceOperation op;
    qint64 space = 0;
    qint64 device = 0;
};

// Of `table` (`ops`, or the snapshot that took the place of what compaction
// dropped of them), the operations kept, each by its names.
void heldIn(const Library &library, const QString &table, std::set<std::tuple<qint64, qint64, qint64>> &seen, QList<Held> &held) {
    for (QSqlQuery query = ChangeLog::query(
             library, QString("SELECT s.name, d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value, g.name, o.signature, "
                              "k.space, k.device FROM kept_ops AS k CROSS JOIN %1 AS o ON o.space = k.space AND o.device = k.device "
                              "AND o.seq = k.seq JOIN log_ids AS s ON s.id = o.space JOIN log_ids AS d ON d.id = o.device "
                              "JOIN log_ids AS a ON a.id = o.author JOIN log_ids AS i ON i.id = o.id "
                              "LEFT JOIN log_ids AS g ON g.id = o.signer")
                          .arg(table));
         query.next();) {
        const qint64 space = query.value(11).toLongLong();
        const qint64 device = query.value(12).toLongLong();
        const qint64 seq = query.value(2).toLongLong();
        if (!seen.insert({space, device, seq}).second)
            continue;
        held.append({.op = {.space = query.value(0).toString(),
                            .device = query.value(1).toString(),
                            .seq = seq,
                            .hlc = query.value(3).toLongLong(),
                            .author = query.value(4).toString(),
                            .entity = query.value(5).toString(),
                            .id = query.value(6).toString(),
                            .field = query.value(7).toString(),
                            .value = query.value(8).toString(),
                            .signer = query.value(9).toString(),
                            .signature = query.value(10).toByteArray()},
                     .space = space,
                     .device = device});
    }
}

[[nodiscard]] QList<Held> held(const Library &library) {
    std::set<std::tuple<qint64, qint64, qint64>> seen;
    QList<Held> held;
    heldIn(library, u"ops"_s, seen, held);
    heldIn(library, u"snapshot_ops"_s, seen, held);
    std::ranges::sort(held, [](const Held &one, const Held &other) {
        return std::tie(one.op.space, one.op.device, one.op.seq) < std::tie(other.op.space, other.op.device, other.op.seq);
    });
    return held;
}

} // namespace

QList<SpaceOperation> KeptOps::keep(const Library &library, QList<SpaceOperation> ops, bool dryRun) {
    QList<SpaceOperation> unknown;
    ops.removeIf([&unknown](const SpaceOperation &op) {
        const bool unknownKind = !OpKinds::known(op);
        if (unknownKind)
            unknown << op;
        return unknownKind;
    });
    if (dryRun || unknown.isEmpty())
        return ops;
    const ChangeLog::Scope guard;
    const Library::Batch batch(library);
    SpaceReplay::store(library, unknown);
    for (const SpaceOperation &op : std::as_const(unknown))
        ChangeLog::exec(library, "INSERT OR IGNORE INTO kept_ops (space, device, seq) VALUES (?, ?, ?)",
                        {ChangeLog::internId(library, op.space), ChangeLog::internId(library, op.device), op.seq});
    qCInfo(lcSync).noquote() << unknown.size() << "operations of a kind this Omabook doesn't know kept, to be taken in once it does";
    return ops;
}

QList<SpaceOperation> KeptOps::kept(const Library &library) {
    QList<SpaceOperation> ops;
    for (const Held &one : held(library))
        ops << one.op;
    return ops;
}

bool KeptOps::takeWith(Taker given) noexcept {
    taker() = given;
    return true;
}

void KeptOps::takeKnown(Library &library) {
    const Taker take = taker();
    if (take == nullptr || !ChangeLog::statements(library).first("SELECT 1 FROM kept_ops LIMIT 1"))
        return;
    const Library::Batch batch(library);
    QList<SpaceOperation> known;
    for (const Held &one : held(library)) {
        if (!OpKinds::known(one.op))
            continue;
        ChangeLog::exec(library, "DELETE FROM kept_ops WHERE space = ? AND device = ? AND seq = ?", {one.space, one.device, one.op.seq});
        known << one.op;
    }
    if (known.isEmpty())
        return;
    qCInfo(lcLibrary).noquote() << "taking in" << known.size() << "operations kept until this Omabook knew their kind";
    take(library, known);
}
