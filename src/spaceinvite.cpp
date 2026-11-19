#include "spaceinvite.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace Qt::StringLiterals;

constexpr auto format = "2.0";
constexpr std::string_view signedMagic{"omabook-invite\x02", 15};
constexpr qsizetype signatureSize = crypto_sign_BYTES;
constexpr qsizetype keySize = SpaceKey::size;
constexpr std::size_t mostKeys = 255; // their count is one byte
constexpr auto base64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

using Wiped = Crypto::WipedBytes;

// What the inviter signs: the name and version, the keys (their count, then
// each), then the invite.
[[nodiscard]] QByteArray signedBytes(QByteArrayView keys, QByteArrayView payload) {
    return QByteArray(signedMagic.data(), signedMagic.size()) + keys + payload;
}

// The keys' part of what is sealed: their count, then each key's bytes.
[[nodiscard]] QByteArray keysPart(std::span<const SpaceKey> keys) {
    QByteArray part(1, static_cast<char>(keys.size()));
    part.reserve(1 + (static_cast<qsizetype>(keys.size()) * keySize)); // grown in place: no copy of a key left behind
    for (const SpaceKey &key : keys) {
        const Wiped bytes(Crypto::toByteArray(key.bytes()));
        part += bytes.bytes();
    }
    return part;
}

// The keys `all` starts with (keysPart), and how many bytes they take.
[[nodiscard]] std::expected<std::pair<std::vector<SpaceKey>, qsizetype>, QString> keysFrom(QByteArrayView all) {
    if (all.isEmpty())
        return std::unexpected(u"This invite is cut short."_s);
    const qsizetype count = static_cast<unsigned char>(all.front());
    const qsizetype size = 1 + (count * keySize);
    if (count == 0 || all.size() < size)
        return std::unexpected(u"This invite is cut short."_s);
    std::vector<SpaceKey> keys;
    for (qsizetype at = 1; at < size; at += keySize) {
        auto key = SpaceKey::fromBytes(Crypto::bytes(all.sliced(at, keySize)));
        if (!key)
            return std::unexpected(key.error());
        keys.push_back(std::move(*key));
    }
    return std::pair{std::move(keys), size};
}

[[nodiscard]] std::expected<SpaceInvite, QString> inviteFrom(const QJsonObject &payload) {
    SpaceInvite invite{.space = payload.value("space").toString(),
                       .name = payload.value("name").toString(),
                       .role = payload.value("role").toString(),
                       .from = payload.value("from").toString(),
                       .to = payload.value("to").toString()};
    if (!invite.space.startsWith("shared:"_L1) || (invite.role != "reader"_L1 && invite.role != "writer"_L1))
        return std::unexpected(u"This invite doesn't name a shared space and a role in it."_s);
    return invite;
}

} // namespace

std::expected<QByteArray, QString> SpaceInvites::write(const Person &from, const SpaceInvite &invite, std::span<const SpaceKey> keys) {
    const auto to = Person::publicKeyFrom(invite.to);
    if (!to)
        return std::unexpected(to.error());
    if (keys.empty() || keys.size() > mostKeys)
        return std::unexpected(u"An invite hands over from one to %1 keys of a space, not %2."_s.arg(mostKeys).arg(keys.size()));
    // Inside the seal: the signature, the keys' bytes, then the invite as
    // JSON, so a key is never text the program can't wipe.
    const QByteArray payload =
        QJsonDocument(
            QJsonObject{{"space", invite.space}, {"name", invite.name}, {"role", invite.role}, {"from", from.id()}, {"to", invite.to}})
            .toJson(QJsonDocument::Compact);
    const Wiped keyBytes(keysPart(keys));
    const Wiped signedPart(signedBytes(keyBytes.bytes(), payload));
    const Person::Signature signature = from.sign(Crypto::bytes(signedPart.bytes()));
    const Wiped inner(Crypto::toByteArray(signature) + keyBytes.bytes() + payload);
    const auto sealed = Person::sealFor(*to, inner.bytes());
    if (!sealed)
        return std::unexpected(sealed.error());
    return QJsonDocument(
               QJsonObject{{"omabook-invite", format}, {"to", invite.to}, {"sealed", QString::fromLatin1(sealed->toBase64(base64))}})
        .toJson(QJsonDocument::Indented);
}

std::expected<SpaceInvites::Opened, QString> SpaceInvites::open(const Person &me, QByteArrayView file) {
    QJsonParseError error{};
    const QJsonObject outer = QJsonDocument::fromJson(file.toByteArray(), &error).object();
    if (error.error != QJsonParseError::NoError || !outer.contains("omabook-invite"))
        return std::unexpected(u"This is not an Omabook invite."_s);
    if (outer.value("omabook-invite").toString().startsWith("1."_L1))
        return std::unexpected(u"This invite was written by an Omabook from before a space's key could change; ask for a new one."_s);
    if (!outer.value("omabook-invite").toString().startsWith("2."_L1))
        return std::unexpected(u"This invite is of a newer kind (%1) than this Omabook knows; update Omabook."_s.arg(
            outer.value("omabook-invite").toString()));
    if (outer.value("to").toString() != me.id())
        return std::unexpected(u"This invite is for someone else (%1), not for you."_s.arg(outer.value("to").toString()));
    auto opened = me.openSealed(QByteArray::fromBase64(outer.value("sealed").toString().toLatin1(), QByteArray::Base64UrlEncoding));
    if (!opened)
        return std::unexpected(opened.error());
    // Moved, not copied: a copy would share the bytes, and wiping it would wipe a detached copy.
    const Wiped inner(std::move(*opened));
    const QByteArrayView all(inner.bytes());
    if (all.size() < signatureSize)
        return std::unexpected(u"This invite is cut short."_s);
    Person::Signature signature{};
    std::ranges::copy(Crypto::bytes(all.first(signatureSize)), signature.begin());
    auto keys = keysFrom(all.sliced(signatureSize));
    if (!keys)
        return std::unexpected(keys.error());
    const QByteArrayView keyBytes = all.sliced(signatureSize, keys->second);
    const QByteArrayView payloadBytes = all.sliced(signatureSize + keys->second);
    auto invite = inviteFrom(QJsonDocument::fromJson(payloadBytes.toByteArray()).object());
    if (!invite)
        return std::unexpected(invite.error());
    const auto from = Person::publicKeyFrom(invite->from);
    const Wiped signedPart(signedBytes(keyBytes, payloadBytes));
    if (!from || !Person::verify(*from, Crypto::bytes(signedPart.bytes()), signature))
        return std::unexpected(u"This invite is not signed by whom it says it is from."_s);
    if (invite->to != me.id())
        return std::unexpected(u"This invite was meant for someone else, and sealed again for you."_s);
    return Opened{.invite = std::move(*invite), .keys = std::move(keys->first)};
}
