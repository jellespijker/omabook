#include "pairing.h"

#include <QByteArray>
#include <QSysInfo>
#include <cstddef>
#include <sodium.h>
#include <utility>

#include "log.h"
#include "person.h"
#include "publishedlog.h"
#include "sharedspace.h"
#include "spaces.h"
#include "syncidentity.h"

namespace {

using namespace Qt::StringLiterals;

// Whether the other side's person is the one both are to be.
[[nodiscard]] bool theirsIsKept(const Person &mine, const QString &theirs) { return !theirs.isEmpty() && theirs < mine.id(); }

// What this computer was in spaces as the person it gave up goes on as the
// one it is (#50): the published spaces it owns are handed over, and in each
// shared space where it was a writer the person it is now is made one. One
// that can't be now (a key not readable) is tried again at the space's next
// sync, which carries over too.
void carryOver(const Library &library, const QString &was, const QString &now) {
    if (const qint64 moved = PublishedLog::handOver(library, was, now); moved > 0)
        qCInfo(lcSync).noquote() << moved << "published spaces handed over to" << now;
    for (const QString &space : SharedSpace::spaces(library))
        if (const auto carried = SharedSpace::carryOver(library, space); !carried)
            qCWarning(lcSync).noquote() << "not handed over yet:" << carried.error();
}

} // namespace

QJsonObject Pairing::introduction(const Library &library) {
    QJsonObject introduction{{u"signs"_s, 1}, {u"name"_s, QSysInfo::machineHostName()}};
    if (const QString machine = SyncIdentity::machine(Spaces::deviceId(library)); !machine.isEmpty())
        introduction.insert(u"machine"_s, machine);
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
    carryOver(library, was, theirPerson);
    return true;
}
