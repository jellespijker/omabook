#include "spacekey.h"

#include <QDir>
#include <QtEndian>
#include <array>
#include <limits>
#include <string_view>

#include "secretfile.h"

namespace {

using namespace Qt::StringLiterals;
using Stream = crypto_secretstream_xchacha20poly1305_state;

// A batch: these eight bytes (a name and the format's version), the
// secretstream's header, then each part as its length (four bytes, big
// endian) and its sealed bytes. The eight bytes and the space's id are the
// additional data of every part, so a batch opens only as what it was sealed as.
constexpr std::string_view batchMagic{"omaseal\x01", 8};
constexpr std::size_t headerSize = crypto_secretstream_xchacha20poly1305_HEADERBYTES;
constexpr std::size_t lengthSize = sizeof(quint32);
constexpr std::size_t partOverhead = crypto_secretstream_xchacha20poly1305_ABYTES;

// A key file: these eight bytes (a name and the format's version), then the key.
constexpr std::string_view keyFileMagic{"omaskey\x01", 8};
using KeyFileBytes = Crypto::SecretBytes<keyFileMagic.size() + SpaceKey::size>;

[[nodiscard]] QByteArray additionalData(QStringView space) { return QByteArray(batchMagic.data(), batchMagic.size()) + space.toUtf8(); }

void appendPart(QByteArray &batch, Stream &stream, const QByteArray &additional, QByteArrayView message, unsigned char tag) {
    const std::size_t sealedSize = static_cast<std::size_t>(message.size()) + partOverhead;
    Q_ASSERT(sealedSize <= std::numeric_limits<quint32>::max());
    std::array<std::byte, lengthSize> length{};
    qToBigEndian(static_cast<quint32>(sealedSize), length.data());
    batch.append(Crypto::toByteArray(length));
    const qsizetype at = batch.size();
    batch.resize(at + static_cast<qsizetype>(sealedSize));
    const std::span<std::byte> sealed =
        std::as_writable_bytes(std::span(batch.data(), static_cast<std::size_t>(batch.size())).subspan(static_cast<std::size_t>(at)));
    const auto additionalBytes = Crypto::bytes(additional);
    crypto_secretstream_xchacha20poly1305_push(&stream, Crypto::writableUchars(sealed), nullptr, Crypto::uchars(Crypto::bytes(message)),
                                               static_cast<unsigned long long>(message.size()), Crypto::uchars(additionalBytes),
                                               additionalBytes.size(), tag);
}

// One part of the batch from `rest`, opened, with its tag; `rest` moves past it.
[[nodiscard]] std::expected<std::pair<QByteArray, unsigned char>, QString> takePart(std::span<const std::byte> &rest, Stream &stream,
                                                                                    const QByteArray &additional) {
    if (rest.size() < lengthSize)
        return std::unexpected(u"The batch is cut short."_s);
    const std::size_t sealedSize = qFromBigEndian<quint32>(rest.data());
    rest = rest.subspan(lengthSize);
    if (sealedSize < partOverhead || sealedSize > rest.size())
        return std::unexpected(u"The batch is cut short."_s);
    QByteArray message(static_cast<qsizetype>(sealedSize - partOverhead), Qt::Uninitialized);
    unsigned char tag = 0;
    const auto additionalBytes = Crypto::bytes(additional);
    const auto opened = crypto_secretstream_xchacha20poly1305_pull(
        &stream, Crypto::writableUchars(std::as_writable_bytes(std::span(message.data(), static_cast<std::size_t>(message.size())))),
        nullptr, &tag, Crypto::uchars(rest.first(sealedSize)), sealedSize, Crypto::uchars(additionalBytes), additionalBytes.size());
    rest = rest.subspan(sealedSize);
    if (opened != 0)
        return std::unexpected(u"The batch was changed, or sealed with another key or for another space."_s);
    return std::pair{message, tag};
}

} // namespace

std::expected<SpaceKey, QString> SpaceKey::generate() {
    if (!Crypto::ready())
        return std::unexpected(u"libsodium could not be made ready: the system gives it no randomness."_s);
    SpaceKey key;
    crypto_secretstream_xchacha20poly1305_keygen(key.m_key.data());
    return key;
}

std::expected<SpaceKey, QString> SpaceKey::fromBytes(std::span<const std::byte> bytes) {
    if (!Crypto::ready())
        return std::unexpected(u"libsodium could not be made ready: the system gives it no randomness."_s);
    if (bytes.size() != size)
        return std::unexpected(u"A space key is %1 bytes, not %2."_s.arg(size).arg(bytes.size()));
    SpaceKey key;
    std::ranges::copy(bytes, key.m_key.span().begin());
    return key;
}

QByteArray SpaceKey::seal(QStringView space, std::span<const QByteArray> messages) const {
    const QByteArray additional = additionalData(space);
    QByteArray batch(batchMagic.data(), batchMagic.size());
    std::array<std::byte, headerSize> header{};
    Stream stream{};
    crypto_secretstream_xchacha20poly1305_init_push(&stream, Crypto::writableUchars(header), m_key.data());
    batch.append(Crypto::toByteArray(header));
    for (const QByteArray &message : messages)
        appendPart(batch, stream, additional, message, crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
    appendPart(batch, stream, additional, {}, crypto_secretstream_xchacha20poly1305_TAG_FINAL);
    sodium_memzero(&stream, sizeof stream);
    return batch;
}

std::expected<QList<QByteArray>, QString> SpaceKey::open(QStringView space, QByteArrayView batch) const {
    std::span<const std::byte> rest = Crypto::bytes(batch);
    if (rest.size() < batchMagic.size() + headerSize
        || !std::ranges::equal(rest.first(batchMagic.size()), std::as_bytes(std::span(batchMagic))))
        return std::unexpected(u"This is not a sealed batch of a kind this Omabook knows."_s);
    rest = rest.subspan(batchMagic.size());
    Stream stream{};
    if (crypto_secretstream_xchacha20poly1305_init_pull(&stream, Crypto::uchars(rest.first(headerSize)), m_key.data()) != 0)
        return std::unexpected(u"The batch's header is not one this key made."_s);
    rest = rest.subspan(headerSize);
    const QByteArray additional = additionalData(space);
    QList<QByteArray> messages;
    std::expected<QList<QByteArray>, QString> result = std::unexpected(u"The batch is cut short."_s);
    while (!rest.empty()) {
        auto part = takePart(rest, stream, additional);
        if (!part) {
            result = std::unexpected(part.error());
            break;
        }
        if (part->second == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            if (rest.empty() && part->first.isEmpty())
                result = messages;
            else
                result = std::unexpected(u"The batch goes on after its end."_s);
            break;
        }
        if (part->second != crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) {
            result = std::unexpected(u"The batch holds a part of a kind this Omabook doesn't know."_s);
            break;
        }
        messages << part->first;
    }
    sodium_memzero(&stream, sizeof stream);
    return result;
}

QString SpaceKey::fileFor(const QString &folder, QStringView space) {
    QString name;
    for (const QChar c : space)
        name += c.isLetterOrNumber() || c == u'-' || c == u'_' ? c : QChar(u'-');
    return QDir(folder).absoluteFilePath(name + u".key"_s);
}

std::expected<void, QString> SpaceKey::keepIn(const QString &file) const {
    KeyFileBytes bytes;
    std::ranges::copy(std::as_bytes(std::span(keyFileMagic)), bytes.span().begin());
    std::ranges::copy(m_key.span(), bytes.span().subspan(keyFileMagic.size()).begin());
    return SecretFile::keep(file, bytes.span(), SecretFile::Placing::Replace);
}

std::expected<SpaceKey, QString> SpaceKey::load(const QString &file) {
    KeyFileBytes bytes;
    if (auto read = SecretFile::read(file, bytes.span(), u"a space's key file"_s); !read)
        return std::unexpected(read.error());
    if (!std::ranges::equal(bytes.span().first(keyFileMagic.size()), std::as_bytes(std::span(keyFileMagic))))
        return std::unexpected(u"%1 is not a space's key file of a kind this Omabook knows."_s.arg(file));
    return fromBytes(bytes.span().subspan(keyFileMagic.size()));
}
