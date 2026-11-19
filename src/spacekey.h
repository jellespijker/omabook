#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringView>
#include <cstddef>
#include <expected>
#include <span>

#include "crypto.h"
#include "secretfile.h"

// A space's key (ADR 0001, *People and keys* and *Transports are adapters*):
// what a shared space's members hold, so that the stores the operations
// travel through (a synced folder, WebDAV, S3) carry only what they can't
// read or change unnoticed.
//
// A batch is sealed with libsodium's secretstream (XChaCha20-Poly1305): each
// message is one part, in order, and an empty last part says the batch is
// whole. A batch that was changed, reordered, cut short or sealed for another
// space or with another key is refused as a whole.
class SpaceKey {
public:
    static constexpr std::size_t size = crypto_secretstream_xchacha20poly1305_KEYBYTES;

    // A new random key.
    [[nodiscard]] static std::expected<SpaceKey, QString> generate();
    // The key in these bytes (as given to a member on purpose).
    [[nodiscard]] static std::expected<SpaceKey, QString> fromBytes(std::span<const std::byte> bytes);

    // The key's bytes, for giving it to a member: they never leave the
    // computer otherwise.
    [[nodiscard]] std::span<const std::byte, size> bytes() const noexcept { return m_key.span(); }
    // A name for the key that gives nothing of it away (a BLAKE2b hash of it
    // and a purpose), the same wherever it is held: what a key rotation names
    // the key it hands out by (KeyRotation, #49).
    [[nodiscard]] QString id() const;

    // Where the key of `space` is kept in `folder`: one file per space, named
    // by its id, so that only the key's owner holds it (the user's Omabook
    // data folder, never a library folder that may be synced elsewhere).
    [[nodiscard]] static QString fileFor(const QString &folder, QStringView space);
    // Keeps the key in `file`, for its owner alone (SecretFile): in place of
    // any kept there, or, with KeepWhatIsThere, never over one.
    [[nodiscard]] std::expected<void, QString> keepIn(const QString &file,
                                                      SecretFile::Placing placing = SecretFile::Placing::Replace) const;
    // The key kept in `file` (keepIn).
    [[nodiscard]] static std::expected<SpaceKey, QString> load(const QString &file);

    // Seals `messages` as one batch of the space `space`.
    [[nodiscard]] QByteArray seal(QStringView space, std::span<const QByteArray> messages) const;
    // The messages of a batch sealed by seal() for `space` with this key.
    [[nodiscard]] std::expected<QList<QByteArray>, QString> open(QStringView space, QByteArrayView batch) const;

private:
    SpaceKey() = default;

    Crypto::SecretBytes<size> m_key;
};
