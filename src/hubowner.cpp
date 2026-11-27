#include "hubowner.h"

#include <QFileInfo>
#include <algorithm>
#include <utility>

#include "crypto.h"
#include "person.h"

using namespace Qt::StringLiterals;

namespace {

// More vouches than any person's computers ever gave up persons; what is past it is not read.
constexpr qsizetype mostVouches = 64;

} // namespace

QByteArray HubOwner::message(QStringView identity, QStringView key) {
    return "omabook hub owner 1\n" + identity.toUtf8() + '\n' + key.toUtf8() + '\n';
}

QString HubOwner::signature(const Person &person, const QByteArray &message) {
    const Person::Signature made = person.sign(Crypto::bytes(message));
    return QString::fromLatin1(Crypto::toByteArray(made).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool HubOwner::signedBy(const QString &identity, const QByteArray &message, const QString &signature) {
    const auto key = Person::publicKeyFrom(identity);
    const auto decoded = QByteArray::fromBase64Encoding(signature.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals
                                                                                  | QByteArray::AbortOnBase64DecodingErrors);
    Person::Signature given{};
    if (!key || !decoded || std::cmp_not_equal(decoded.decoded.size(), given.size()))
        return false;
    std::ranges::copy(Crypto::bytes(decoded.decoded), given.begin());
    return Person::verify(*key, Crypto::bytes(message), given);
}

QString HubOwner::vouch(const Person &former, QStringView identity, QStringView key) {
    return former.id() + u'.' + signature(former, message(identity, key));
}

bool HubOwner::vouched(const QStringList &owners, QStringView identity, QStringView key, QStringView former) {
    const QStringList words = former.toString().split(u'.');
    const QByteArray said = message(identity, key);
    for (qsizetype at = 0; at + 1 < std::min(words.size(), 2 * mostVouches); at += 2)
        if (owners.contains(words.at(at)) && signedBy(words.at(at), said, words.at(at + 1)))
            return true;
    return false;
}

HubOwner::Claim HubOwner::claim(PersonFile &person, QStringView key) {
    // Nothing given up, nothing to say; and no person is made here for asking.
    const QStringList given = person.formerIds();
    if (given.isEmpty() || !QFileInfo::exists(person.file()))
        return {};
    const Person *now = person.get();
    if (now == nullptr)
        return {};
    const QString identity = now->id();
    QStringList vouches;
    for (const QString &id : given)
        if (const auto former = person.former(id); former && id != identity && vouches.size() < mostVouches)
            vouches << vouch(*former, identity, key);
    if (vouches.isEmpty())
        return {};
    return {.identity = identity, .signature = signature(*now, message(identity, key)), .former = vouches.join(u'.')};
}
