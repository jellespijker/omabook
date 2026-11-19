#include "spaceinvite.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace {

using namespace Qt::StringLiterals;

constexpr auto format = "1.0";
constexpr std::string_view signedMagic{"omabook-invite\x01", 15};
constexpr auto base64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

// Bytes that held a secret, wiped when they go.
class Wiped {
public:
    explicit Wiped(QByteArray bytes) noexcept : m_bytes(std::move(bytes)) {}
    ~Wiped() { sodium_memzero(m_bytes.data(), static_cast<std::size_t>(m_bytes.size())); }
    Wiped(const Wiped &) = delete;
    Wiped &operator=(const Wiped &) = delete;
    Wiped(Wiped &&) = delete;
    Wiped &operator=(Wiped &&) = delete;

    [[nodiscard]] const QByteArray &bytes() const noexcept { return m_bytes; }
    [[nodiscard]] QByteArray &bytes() noexcept { return m_bytes; }

private:
    QByteArray m_bytes;
};

// What the inviter signs: the name and version, the key, then the invite.
[[nodiscard]] QByteArray signedBytes(QByteArrayView key, QByteArrayView payload) {
    return QByteArray(signedMagic.data(), signedMagic.size()) + key + payload;
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

std::expected<QByteArray, QString> SpaceInvites::write(const Person &from, const SpaceInvite &invite, const SpaceKey &key) {
    const auto to = Person::publicKeyFrom(invite.to);
    if (!to)
        return std::unexpected(to.error());
    // Inside the seal: the signature, the key's bytes, then the invite as
    // JSON, so the key is never text the program can't wipe.
    const QByteArray payload =
        QJsonDocument(
            QJsonObject{{"space", invite.space}, {"name", invite.name}, {"role", invite.role}, {"from", from.id()}, {"to", invite.to}})
            .toJson(QJsonDocument::Compact);
    const Wiped keyBytes(Crypto::toByteArray(key.bytes()));
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
    if (!outer.value("omabook-invite").toString().startsWith("1."_L1))
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
    constexpr qsizetype signatureSize = crypto_sign_BYTES;
    constexpr qsizetype keySize = SpaceKey::size;
    if (all.size() < signatureSize + keySize)
        return std::unexpected(u"This invite is cut short."_s);
    Person::Signature signature{};
    std::ranges::copy(Crypto::bytes(all.first(signatureSize)), signature.begin());
    const QByteArrayView keyBytes = all.sliced(signatureSize, keySize);
    const QByteArrayView payloadBytes = all.sliced(signatureSize + keySize);
    auto invite = inviteFrom(QJsonDocument::fromJson(payloadBytes.toByteArray()).object());
    if (!invite)
        return std::unexpected(invite.error());
    const auto from = Person::publicKeyFrom(invite->from);
    const Wiped signedPart(signedBytes(keyBytes, payloadBytes));
    if (!from || !Person::verify(*from, Crypto::bytes(signedPart.bytes()), signature))
        return std::unexpected(u"This invite is not signed by whom it says it is from."_s);
    if (invite->to != me.id())
        return std::unexpected(u"This invite was meant for someone else, and sealed again for you."_s);
    auto key = SpaceKey::fromBytes(Crypto::bytes(keyBytes));
    if (!key)
        return std::unexpected(key.error());
    return Opened{.invite = std::move(*invite), .key = std::move(*key)};
}
