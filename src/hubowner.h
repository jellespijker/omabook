#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringView>

class Person;
class PersonFile;

// Whose a hub is, and how its owner goes on being its owner (#283, ADR 0012).
// A hub belongs to one person, named by the first computer linked to it. That
// person's id changes when their computers pair (Pairing: the smaller id wins),
// so a hub also knows the ids its owner has gone by since. It takes a new one
// only from a computer that holds the secret keys of both: the new one, and one
// the hub already knows as its owner's, which a computer that gave it up still
// keeps (PersonFile::former). What the old key signs names the new id and the
// computer's key for the hub, so it opens nothing for anyone else's computer.
namespace HubOwner {

// What a key signs to say that its person is `identity` now, for the computer
// whose key for the hub is the line `key`.
[[nodiscard]] QByteArray message(QStringView identity, QStringView key);
// `person`'s signature of `message`, in URL-safe Base64.
[[nodiscard]] QString signature(const Person &person, const QByteArray &message);
[[nodiscard]] bool signedBy(const QString &identity, const QByteArray &message, const QString &signature);

// One former person's word that they are `identity` now: "<their id>.<signature>".
[[nodiscard]] QString vouch(const Person &former, QStringView identity, QStringView key);
// Whether `former`, vouches joined by dots, holds one of a person among `owners`
// for `identity` and `key`.
[[nodiscard]] bool vouched(const QStringList &owners, QStringView identity, QStringView key, QStringView former);

// What a computer that gave up earlier persons says of who it is now: its id,
// that person's signature, and the vouches of those given up. All empty when
// it gave none up, or none can be read.
struct Claim {
    QString identity, signature, former;
};
[[nodiscard]] Claim claim(PersonFile &person, QStringView key);

} // namespace HubOwner
