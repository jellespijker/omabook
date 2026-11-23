#include "syncidentity.h"

#include <QList>
#include <QUuid>
#include <algorithm>

#include "changelog.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

QStringList SyncIdentity::machineColumn() {
    return {u"DROP TABLE IF EXISTS space_members_next"_s,
            u"CREATE TABLE space_members_next (space TEXT NOT NULL REFERENCES spaces(id) ON DELETE CASCADE, person TEXT NOT NULL, "
            "role TEXT NOT NULL DEFAULT 'writer', name TEXT NOT NULL DEFAULT '', added INTEGER NOT NULL DEFAULT 0, "
            "machine TEXT NOT NULL DEFAULT '', PRIMARY KEY (space, person))"_s,
            u"INSERT INTO space_members_next (space, person, role, name, added) SELECT space, person, role, name, added "
            "FROM space_members"_s,
            u"DROP TABLE space_members"_s, u"ALTER TABLE space_members_next RENAME TO space_members"_s};
}

std::expected<void, Failure> SyncIdentity::check(const Library &library, const QJsonObject &theirs, const QString &computer) {
    const QString device = theirs.value("device"_L1).toString();
    if (device.isEmpty())
        return {};
    if (device == Spaces::deviceId(library))
        return std::unexpected(Failure(Failure::Code::SyncDeviceOurs, {computer}));
    if (const QString name = theirs.value("name"_L1).toString(); !name.isEmpty()) {
        const QList<Spaces::Computer> known = Spaces::computers(library);
        const auto other = std::ranges::find_if(
            known, [&](const Spaces::Computer &one) { return one.device == device && !one.name.isEmpty() && one.name != name; });
        if (other != known.cend())
            return std::unexpected(Failure(Failure::Code::SyncDeviceTaken, {computer, other->name}));
    }
    return {};
}

SyncIdentity::Renewed SyncIdentity::renew(const Library &library) {
    const Library::Batch batch(library);
    Renewed renewed{.was = Spaces::deviceId(library), .now = QUuid::createUuid().toString(QUuid::Id128)};
    ChangeLog::exec(library,
                    "INSERT OR IGNORE INTO space_members (space, person, role, added) SELECT space, ?, role, added FROM space_members "
                    "WHERE person = ?",
                    {renewed.now, renewed.was});
    Spaces::setGlobalId(library, u"device"_s, 0, renewed.now);
    return renewed;
}
