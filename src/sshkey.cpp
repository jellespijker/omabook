#include "sshkey.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <span>
#include <utility>

#include "crypto.h"
#include "secretfile.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto ed25519 = "ssh-ed25519";
constexpr qsizetype publicBytes = crypto_sign_PUBLICKEYBYTES;
// OpenSSH's private key file wraps its Base64 at 70 characters.
constexpr qsizetype wrapAt = 70;
// The cipher "none" pads its private part to blocks of 8.
constexpr qsizetype blockSize = 8;

void putUint32(QByteArray &out, quint32 value) {
    for (const unsigned shift : {24U, 16U, 8U, 0U})
        out.append(static_cast<char>((value >> shift) & 0xFFU));
}

// A string as ssh writes one: its length, then its bytes.
void putString(QByteArray &out, QByteArrayView bytes) {
    putUint32(out, static_cast<quint32>(bytes.size()));
    out.append(bytes);
}

[[nodiscard]] QByteArrayView view(std::span<const std::byte> bytes) { return {bytes.data(), static_cast<qsizetype>(bytes.size())}; }

// The public key as ssh sends it: the type and the key, each a string.
[[nodiscard]] QByteArray wire(std::span<const std::byte> key) {
    QByteArray blob;
    putString(blob, ed25519);
    putString(blob, view(key));
    return blob;
}

[[nodiscard]] Failure unmade(const QString &file, const QString &why) { return {Failure::Code::HubKeyUnmade, {file, why}}; }

// OpenSSH's private key file ("openssh-key-v1", unencrypted: the file is the
// owner's alone), as PEM text. Every buffer that holds the secret is sized
// before it is filled, so that no copy is left behind as it grows, and wiped.
[[nodiscard]] Crypto::WipedBytes privateFile(const SshKey::Public &key, std::span<const std::byte> secret, const QByteArray &comment) {
    QByteArray inner;
    inner.reserve(512 + comment.size());
    const quint32 check = randombytes_random();
    putUint32(inner, check);
    putUint32(inner, check);
    putString(inner, key.type);
    putString(inner, view(secret).last(publicBytes));
    putString(inner, view(secret));
    putString(inner, comment);
    for (char pad = 1; inner.size() % blockSize != 0; ++pad)
        inner.append(pad);
    const Crypto::WipedBytes innerKept(std::move(inner));
    QByteArray whole;
    whole.reserve(1024 + comment.size());
    whole.append("openssh-key-v1");
    whole.append('\0');
    putString(whole, "none"); // cipher
    putString(whole, "none"); // key derivation
    putString(whole, "");     // its options
    putUint32(whole, 1);      // one key
    putString(whole, key.blob);
    putString(whole, innerKept.bytes());
    const Crypto::WipedBytes wholeKept(std::move(whole));
    const Crypto::WipedBytes encoded(wholeKept.bytes().toBase64());
    QByteArray text;
    text.reserve(encoded.bytes().size() * 2 + 128);
    text.append("-----BEGIN OPENSSH PRIVATE KEY-----\n");
    for (qsizetype at = 0; at < encoded.bytes().size(); at += wrapAt) {
        text.append(QByteArrayView(encoded.bytes()).sliced(at, std::min(wrapAt, encoded.bytes().size() - at)));
        text.append('\n');
    }
    text.append("-----END OPENSSH PRIVATE KEY-----\n");
    return Crypto::WipedBytes(std::move(text));
}

} // namespace

namespace SshKey {

std::expected<Public, Failure> generate(const QString &file, const QString &comment) {
    if (!Crypto::ready())
        return std::unexpected(unmade(file, qt_error_string(EIO)));
    std::array<std::byte, crypto_sign_PUBLICKEYBYTES> publicKey{};
    Crypto::SecretBytes<crypto_sign_SECRETKEYBYTES> secret;
    if (crypto_sign_keypair(Crypto::writableUchars(publicKey), secret.data()) != 0)
        return std::unexpected(unmade(file, qt_error_string(EIO)));
    Public key{.type = ed25519, .blob = wire(publicKey)};
    const Crypto::WipedBytes text = privateFile(key, secret.span(), comment.toUtf8());
    if (const auto kept = SecretFile::keep(file, Crypto::bytes(text.bytes()), SecretFile::Placing::KeepWhatIsThere); !kept)
        return std::unexpected(unmade(file, kept.error()));
    QSaveFile pub(file + u".pub"_s);
    if (!pub.open(QIODevice::WriteOnly) || pub.write((line(key) + ' ' + comment + '\n').toUtf8()) < 0 || !pub.commit())
        return std::unexpected(unmade(file + u".pub"_s, pub.errorString()));
    return key;
}

std::expected<Public, Failure> publicOf(const QString &file) {
    QFile pub(file + u".pub"_s);
    if (!pub.open(QIODevice::ReadOnly))
        return std::unexpected(unmade(pub.fileName(), pub.errorString()));
    return parse(QString::fromUtf8(pub.read(4096)).trimmed());
}

std::expected<Public, Failure> parse(QStringView line) {
    const auto bad = std::unexpected(Failure(Failure::Code::HubBadKey));
    if (std::ranges::any_of(line, [](QChar c) { return c.category() == QChar::Other_Control; }))
        return bad;
    const QList<QStringView> words = line.trimmed().split(u' ', Qt::SkipEmptyParts);
    if (words.size() < 2 || words.at(0) != QLatin1StringView(ed25519))
        return bad;
    const auto decoded = QByteArray::fromBase64Encoding(words.at(1).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return bad;
    // One ed25519 key, and nothing after it.
    const std::array<std::byte, crypto_sign_PUBLICKEYBYTES> nothing{};
    const QByteArray expected = wire(nothing);
    if (decoded.decoded.size() != expected.size() || !decoded.decoded.startsWith(expected.first(expected.size() - publicBytes)))
        return bad;
    return Public{.type = ed25519, .blob = decoded.decoded};
}

QString line(const Public &key) { return QString::fromLatin1(key.type) + ' ' + QString::fromLatin1(key.blob.toBase64()); }

QString fingerprint(const Public &key) {
    return u"SHA256:"_s
           + QString::fromLatin1(QCryptographicHash::hash(key.blob, QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals));
}

} // namespace SshKey
