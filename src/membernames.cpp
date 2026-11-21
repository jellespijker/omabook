#include "membernames.h"

#include <optional>

#include "untrustedtext.h"

QHash<QString, QString> MemberNames::of(std::span<const SpaceOperation> ops) {
    QHash<QString, QString> names;
    QHash<QString, qint64> at;
    for (const SpaceOperation &op : ops) {
        if (op.entity != QLatin1StringView(entity) || op.field != QLatin1StringView(field) || op.id.isEmpty() || op.author != op.id)
            continue;
        if (at.contains(op.id) && at.value(op.id) > op.hlc)
            continue;
        // An empty name takes the one before away; one that isn't a name (too long) is passed over, the one before standing.
        if (UntrustedText::plain(op.value).trimmed().isEmpty()) {
            at.insert(op.id, op.hlc);
            names.remove(op.id);
        } else if (const std::optional<QString> name = UntrustedText::name(op.value)) {
            at.insert(op.id, op.hlc);
            names.insert(op.id, *name);
        }
    }
    return names;
}

ChangeLog::Entry MemberNames::entry(const QString &space, const QString &person, const QString &name) {
    return {
        .space = space, .entity = QString::fromLatin1(entity), .id = person, .field = QString::fromLatin1(field), .value = name, .hlc = {}};
}
