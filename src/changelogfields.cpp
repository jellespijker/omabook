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
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT o.field, o.value, o.hlc, d.name FROM ops AS o JOIN log_ids AS d ON d.id = o.device "
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

void ChangeLogFields::write(Context &context, const QString &field, const QString &value, qint64 rowHlc) {
    const auto it = context.existing.constFind(field);
    if (it != context.existing.cend() && it->own.value_or(it->value) == value)
        return;
    qint64 useHlc = rowHlc;
    if (!context.baseline) {
        const bool isRecentWall =
            rowHlc > 0 && Hlc::counter(rowHlc) == 0 && std::abs(Spaces::wallSeconds(*context.library) - Hlc::seconds(rowHlc)) <= 5;
        if (isRecentWall || (it != context.existing.cend() && useHlc <= it->hlc))
            useHlc = Spaces::tickHlc(*context.library);
    } else if (it != context.existing.cend() && useHlc <= it->hlc) {
        useHlc = Spaces::tickHlc(*context.library);
    }
    ChangeLog::append(*context.library, {context.space, context.entity, context.gid, field, value, useHlc});
    context.existing.insert(field, {useHlc, Spaces::deviceId(*context.library), value, value});
}
