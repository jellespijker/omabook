#include "syncidentity.h"

#include <QCryptographicHash>
#include <QFile>
#include <QList>
#include <QUuid>
#include <algorithm>

#include "changelog.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

QString SyncIdentity::machine(const QString &device) {
    QFile file(u"/etc/machine-id"_s);
    return file.open(QIODevice::ReadOnly) ? machineOf(file.readAll(), device) : QString();
}

QStringList SyncIdentity::machineColumn() {
    return {u"DROP TABLE IF EXISTS space_members_next"_s,
            u"CREATE TABLE space_members_next (space TEXT NOT NULL REFERENCES spaces(id) ON DELETE CASCADE, person TEXT NOT NULL, "
            "role TEXT NOT NULL DEFAULT 'writer', name TEXT NOT NULL DEFAULT '', added INTEGER NOT NULL DEFAULT 0, "
            "machine TEXT NOT NULL DEFAULT '', PRIMARY KEY (space, person))"_s,
            u"INSERT INTO space_members_next (space, person, role, name, added) SELECT space, person, role, name, added "
            "FROM space_members"_s,
            u"DROP TABLE space_members"_s, u"ALTER TABLE space_members_next RENAME TO space_members"_s};
}

QString SyncIdentity::machineOf(const QByteArray &machineId, const QString &device) {
    const QByteArray id = machineId.trimmed();
    if (id.isEmpty())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("omabook machine\x1f"));
    hash.addData(device.toUtf8());
    hash.addData(QByteArrayView("\x1f"));
    hash.addData(id);
    return QString::fromLatin1(hash.result().toHex().left(32));
}

std::expected<SyncIdentity::Known, Failure> SyncIdentity::check(const Library &library, const QJsonObject &theirs,
                                                                const QString &computer) {
    const QString device = theirs.value("device"_L1).toString();
    if (device.isEmpty())
        return Known{};
    if (device == Spaces::deviceId(library))
        return std::unexpected(Failure(Failure::Code::SyncDeviceOurs, {computer}));
    const QList<Spaces::Computer> known = Spaces::computers(library);
    const auto other = std::ranges::find(known, device, &Spaces::Computer::device);
    if (other == known.cend())
        return Known{};
    const QString name = theirs.value("name"_L1).toString();
    const bool renamed = !name.isEmpty() && !other->name.isEmpty() && other->name != name;
    const QString machine = theirs.value("machine"_L1).toString();
    // By its machine where both say one; else by its name, as before #215.
    const bool another = machine.isEmpty() || other->machine.isEmpty() ? renamed : machine != other->machine;
    if (another)
        return std::unexpected(Failure(Failure::Code::SyncDeviceTaken, {computer, other->name.isEmpty() ? device : other->name}));
    return Known{.renamedFrom = renamed ? other->name : QString()};
}

QJsonObject SyncIdentity::answer(const Library &library, const QJsonObject &theirs) {
    const auto checked = check(library, theirs, theirs.value("name"_L1).toString());
    if (!checked)
        return {{"refused", checked.error().text()}};
    return checked->renamedFrom.isEmpty() ? QJsonObject() : QJsonObject{{"renamed", checked->renamedFrom}};
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
