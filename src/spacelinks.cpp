#include "spacelinks.h"

#include <QSqlQuery>
#include <QVariant>

#include "changelog.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString nameOf(SpaceLinks::Role role) {
    switch (role) {
    case SpaceLinks::Role::Sent:
        return u"sent"_s;
    case SpaceLinks::Role::Followed:
        return u"followed"_s;
    case SpaceLinks::Role::Files:
        return u"files"_s;
    }
    return {};
}

} // namespace

void SpaceLinks::remember(const Library &library, Role role, const Link &link) {
    if (role != Role::Sent)
        ChangeLog::exec(library, "DELETE FROM space_links WHERE space = ? AND role = ?", {link.space, nameOf(role)});
    ChangeLog::exec(library,
                    "INSERT INTO space_links (space, role, transport, address) VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(space, role, transport) DO UPDATE SET address = excluded.address",
                    {link.space, nameOf(role), link.transport, link.address});
}

std::optional<SpaceLinks::Link> SpaceLinks::of(const Library &library, const QString &space, Role role, const QString &transport) {
    QSqlQuery query = ChangeLog::query(
        library,
        "SELECT transport, address FROM space_links WHERE space = ? AND role = ? AND (COALESCE(?, '') = '' OR transport = ?) "
        "ORDER BY transport LIMIT 1",
        {space, nameOf(role), transport, transport});
    if (!query.next())
        return std::nullopt;
    return Link{.space = space, .transport = query.value(0).toString(), .address = query.value(1).toString()};
}

QList<SpaceLinks::Link> SpaceLinks::all(const Library &library, Role role) {
    QList<Link> links;
    for (QSqlQuery query =
             ChangeLog::query(library, "SELECT space, transport, address FROM space_links WHERE role = ? ORDER BY space", {nameOf(role)});
         query.next();)
        links << Link{.space = query.value(0).toString(), .transport = query.value(1).toString(), .address = query.value(2).toString()};
    return links;
}

void SpaceLinks::forget(const Library &library, const QString &space) {
    ChangeLog::exec(library, "DELETE FROM space_links WHERE space = ?", {space});
}
