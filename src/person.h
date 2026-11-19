#pragma once

#include <QString>
#include <QStringView>
#include <array>
#include <cstddef>
#include <expected>
#include <span>

#include "crypto.h"

// A person is a key pair (ADR 0001, *People and keys*): Ed25519, with
// libsodium. The public key is the person's id; the secret key signs what
// the person writes and never leaves the computer. It is made on first use
// and kept in a file beside the library that only its owner may read.
class Person {
public:
    using PublicKey = std::array<std::byte, crypto_sign_PUBLICKEYBYTES>;
    using Signature = std::array<std::byte, crypto_sign_BYTES>;

    // The key file of the library in `libraryRoot`, beside its omabook.db.
    [[nodiscard]] static QString keyFile(const QString &libraryRoot);

    // The person kept in `file`, made and kept there first when there is
    // none. Two programs that make one at once keep the same: the second
    // finds the first's and reads it.
    [[nodiscard]] static std::expected<Person, QString> loadOrCreate(const QString &file);
    // The person kept in `file`. Refused when the file can be read or
    // written by anyone but its owner, or is not a key file of this kind.
    [[nodiscard]] static std::expected<Person, QString> load(const QString &file);
    // A new person, kept nowhere (what a test or a pairing starts from).
    [[nodiscard]] static std::expected<Person, QString> generate();

    [[nodiscard]] const PublicKey &publicKey() const noexcept { return m_public; }
    [[nodiscard]] QString id() const { return idOf(m_public); }
    [[nodiscard]] QString fingerprint() const { return fingerprintOf(m_public); }
    // What may be given to others: the public key, written as the id.
    [[nodiscard]] QString exportPublicKey() const { return id(); }

    [[nodiscard]] Signature sign(std::span<const std::byte> message) const;

    // The id of a public key: the key in URL-safe base64 without padding.
    [[nodiscard]] static QString idOf(const PublicKey &key);
    // A short hash of the key, in groups of four hex digits, to compare by
    // eye or read out to someone.
    [[nodiscard]] static QString fingerprintOf(const PublicKey &key);
    // The public key an id (or an exported key) names.
    [[nodiscard]] static std::expected<PublicKey, QString> publicKeyFrom(QStringView id);
    [[nodiscard]] static bool verify(const PublicKey &key, std::span<const std::byte> message, const Signature &signature);

private:
    using Seed = Crypto::SecretBytes<crypto_sign_SEEDBYTES>;
    using SecretKey = Crypto::SecretBytes<crypto_sign_SECRETKEYBYTES>;

    Person() = default;
    [[nodiscard]] static std::expected<Person, QString> fromSeed(const Seed &seed);
    [[nodiscard]] static std::expected<void, QString> keep(const QString &file, const Seed &seed);

    PublicKey m_public{};
    SecretKey m_secret;
};
