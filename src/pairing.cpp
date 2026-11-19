#include "pairing.h"

#include <QByteArray>
#include <QSysInfo>
#include <cstddef>
#include <sodium.h>
#include <utility>

#include "log.h"
#include "person.h"

namespace {

using namespace Qt::StringLiterals;

// Whether the other side's person is the one both are to be.
[[nodiscard]] bool theirsIsKept(const Person &mine, const QString &theirs) { return !theirs.isEmpty() && theirs < mine.id(); }

} // namespace

QJsonObject Pairing::introduction(const Library &library) {
    QJsonObject introduction{{u"signs"_s, 1}, {u"name"_s, QSysInfo::machineHostName()}};
    if (const Person *person = library.person())
        introduction.insert(u"person"_s, person->id());
    return introduction;
}

QJsonObject Pairing::keyFor(const Library &library, const QJsonObject &theirs) {
    const Person *mine = library.person();
    const QString theirPerson = theirs.value(u"person"_s).toString();
    if (mine == nullptr || theirs.value(u"signs"_s).toInt() < 1 || theirPerson.isEmpty() || !(mine->id() < theirPerson))
        return {};
    QByteArray secret = mine->secretForPairing();
    QJsonObject given{{u"person_key"_s, QString::fromLatin1(secret)}};
    // What is left of it here is in the JSON alone, which goes as soon as it is sent.
    sodium_memzero(secret.data(), static_cast<std::size_t>(secret.size()));
    return given;
}

std::expected<bool, QString> Pairing::take(Library &library, const QJsonObject &theirs) {
    const Person *mine = library.person();
    const QString theirPerson = theirs.value(u"person"_s).toString();
    if (mine == nullptr || !theirsIsKept(*mine, theirPerson))
        return false;
    QByteArray secret = theirs.value(u"person_key"_s).toString().toLatin1();
    if (secret.isEmpty())
        return std::unexpected(u"The other computer's person was not given, so this one stays who it was for now."_s);
    auto person = Person::fromPairingSecret(secret);
    sodium_memzero(secret.data(), static_cast<std::size_t>(secret.size()));
    if (!person)
        return std::unexpected(person.error());
    if (person->id() != theirPerson)
        return std::unexpected(u"The key the other computer gave is not of the person it says it is."_s);
    const QString was = mine->id();
    if (auto kept = library.personFile().adopt(std::move(*person)); !kept)
        return std::unexpected(kept.error());
    qCInfo(lcSync).noquote() << "paired: this computer is now person" << theirPerson << "and no longer" << was;
    return true;
}
