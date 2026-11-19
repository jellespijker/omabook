#include "changelogfields.h"

#include <QSqlQuery>
#include <QVariant>
#include <cstdlib>
#include <tuple>

#include "changelog.h"
#include "hlc.h"
#include "spaces.h"

ChangeLogFields::Map ChangeLogFields::logged(const Library &library, const QString &entity, const QString &globalId) {
    Map map;
    const QString me = Spaces::deviceId(library);
    for (QSqlQuery query =
             ChangeLog::query(library,
                              "SELECT o.field, o.value, o.hlc, d.name FROM live_ops AS o JOIN log_ids AS d ON d.id = o.device "
                              "WHERE o.entity = ? AND o.id = (SELECT id FROM log_ids WHERE name = ?) ORDER BY o.seq",
                              {entity, globalId});
         query.next();) {
        const QString field = query.value(0).toString();
        const QString value = query.value(1).toString();
        const qint64 hlc = query.value(2).toLongLong();
        const QString device = query.value(3).toString();
        Logged &latest = map[field];
        if (std::tie(hlc, device, value) >= std::tie(latest.hlc, latest.device, latest.value))
            std::tie(latest.hlc, latest.device, latest.value) = std::tie(hlc, device, value);
        if (device == me) // in the order this device wrote them
            latest.own = value;
    }
    return map;
}

namespace {

// The stamp of a value new on this device: the row's own time, unless that is
// a write just now or no later than what the log has of the field, which takes
// a new tick.
[[nodiscard]] qint64 newStamp(const ChangeLogFields::Context &context, const ChangeLogFields::Logged *logged, qint64 rowHlc) {
    const bool isRecentWall = !context.baseline && rowHlc > 0 && Hlc::counter(rowHlc) == 0
                              && std::abs(Spaces::wallSeconds(*context.library) - Hlc::seconds(rowHlc)) <= 5;
    return isRecentWall || (logged && rowHlc <= logged->hlc) ? Spaces::tickHlc(*context.library) : rowHlc;
}

} // namespace

void ChangeLogFields::write(Context &context, const QString &field, const QString &value, qint64 rowHlc) {
    const auto it = context.existing.constFind(field);
    const Logged *logged = it != context.existing.cend() ? &*it : nullptr;
    if (logged && logged->own.value_or(logged->value) == value)
        return;
    // A value this device took from another's operation keeps that one's
    // stamp: an echo of it stamped now could beat a newer edit made there
    // meanwhile. At the same stamp and value it changes no outcome.
    const qint64 useHlc = logged && logged->value == value ? logged->hlc : newStamp(context, logged, rowHlc);
    ChangeLog::append(*context.library, {context.space, context.entity, context.gid, field, value, useHlc});
    context.existing.insert(field, {useHlc, Spaces::deviceId(*context.library), value, value});
}
