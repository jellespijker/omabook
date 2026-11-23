#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <expected>

#include "failure.h"

class Library;

// A library's device id is its own (#209, ADR 0001): what it writes in the
// change log is numbered under it, one after another, so two libraries of one
// id (a copy, or a library restored from a snapshot or a backup) would number
// different writes alike, and a computer that has one of them takes the
// other's for writes it holds already. A sync makes sure, before it writes
// anything on either side, that the other library is not this one, nor
// another computer known here; and a library restored or copied is given an
// id of its own.
namespace SyncIdentity {

// What a library says of the machine it runs on (#215): the machine's id
// (/etc/machine-id) hashed with the library's device id, so it is never said
// in clear and no two libraries say one machine alike; empty on a machine
// without one (a container), which is known by its host name alone.
[[nodiscard]] QString machine(const QString &device);
// The step that gives the computers of the personal space the machine each
// says it runs on (format 47), by which a sync knows one renamed; empty until
// it says one. Made anew, so a library given the step again takes it as well.
[[nodiscard]] QStringList machineColumn();
// The same of a machine id given (`machine` reads /etc/machine-id).
[[nodiscard]] QString machineOf(const QByteArray &machineId, const QString &device);

// What a sync is told of a library it may sync with: the name a computer
// known here went by, when its machine (the same) goes by another now.
struct Known {
    QString renamedFrom;
};

// Whether this library may sync with the one `theirs` introduces (a sync's
// manifest or request: its "device", "name" and "machine"), on `computer`.
// Refused when its device id is this library's (SyncDeviceOurs), or that of
// another computer known here (SyncDeviceTaken): on another machine than it
// said before, or, where either says none, by another name. One that says no
// device (an Omabook from before the log) is not refused, nor a library that
// is only behind on its own log: a copy of it put back from a backup, which
// the sync brings up to date (#61). The name it has now is recorded when the
// sync pairs the two (Spaces::pair).
[[nodiscard]] std::expected<Known, Failure> check(const Library &library, const QJsonObject &theirs, const QString &computer);

// What the other side of a sync answers to `sync --identity`: why not, as
// "refused", or the name it knew this computer by, as "renamed" (#215).
[[nodiscard]] QJsonObject answer(const Library &library, const QJsonObject &theirs);

// A device id of its own for the library, in place of the one it had: what it
// wrote stays under the old one, and the new one is a member of each space the
// old one was, so it is one of one's own computers wherever that was.
struct Renewed {
    QString was, now;
};
Renewed renew(const Library &library);

} // namespace SyncIdentity
