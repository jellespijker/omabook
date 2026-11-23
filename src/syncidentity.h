#pragma once

#include <QJsonObject>
#include <QString>
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

// Whether this library may sync with the one `theirs` introduces (a sync's
// manifest or request: its "device" and "name"), on `computer`. Refused when
// its device id is this library's (SyncDeviceOurs), or that of another
// computer known here by another name (SyncDeviceTaken). One that says no
// device (an Omabook from before the log) is not refused, nor a library that
// is only behind on its own log: a copy of it put back from a backup, which
// the sync brings up to date (#61).
[[nodiscard]] std::expected<void, Failure> check(const Library &library, const QJsonObject &theirs, const QString &computer);

// A device id of its own for the library, in place of the one it had: what it
// wrote stays under the old one, and the new one is a member of each space the
// old one was, so it is one of one's own computers wherever that was.
struct Renewed {
    QString was, now;
};
Renewed renew(const Library &library);

} // namespace SyncIdentity
