#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include "crypto.h"

// A person is a key pair (ADR 0001, *People and keys*): Ed25519, with
// libsodium. The public key is the person's id; the secret key signs what
// the person writes. It is made on first use and kept per user, in a file
// only its owner may read: one person, one key, on every computer of theirs.
// It leaves a computer only when it is paired with another of the person's
// own, over ssh (PersonFile::adopt), or when exported on purpose.
class Person {
public:
    using PublicKey = std::array<std::byte, crypto_sign_PUBLICKEYBYTES>;
    using Signature = std::array<std::byte, crypto_sign_BYTES>;

    // Where the user's person is kept: their Omabook data folder, the same
    // for every library of theirs, never inside a library folder that may
    // itself be synced somewhere.
    [[nodiscard]] static QString userKeyFile();
    // Where the first builds of 0.1.7 kept it: beside the library in
    // `libraryRoot`. A key found there is moved to userKeyFile() once.
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
    // Keeps this person in `file`, in place of whoever was kept there.
    [[nodiscard]] std::expected<void, QString> keepIn(const QString &file) const;

    // The secret, written out for pairing another of one's own computers
    // over ssh and for nothing else: the seed the key pair is made from, in
    // URL-safe base64. The caller wipes it (sodium_memzero) once it is sent.
    [[nodiscard]] QByteArray secretForPairing() const;
    // The person a secretForPairing() names.
    [[nodiscard]] static std::expected<Person, QString> fromPairingSecret(QByteArrayView secret);

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
    friend class PersonFile; // moves a key kept beside a library to the user's, never over one there
    using Seed = Crypto::SecretBytes<crypto_sign_SEEDBYTES>;
    using SecretKey = Crypto::SecretBytes<crypto_sign_SECRETKEYBYTES>;

    Person() = default;
    enum class Placing : std::uint8_t { KeepWhatIsThere, Replace };

    [[nodiscard]] static std::expected<Person, QString> fromSeed(const Seed &seed);
    [[nodiscard]] static std::expected<void, QString> keep(const QString &file, const Seed &seed, Placing placing);
    [[nodiscard]] Seed seed() const;

    PublicKey m_public{};
    SecretKey m_secret;
};

// The person a library signs with: where they are kept, and the person read
// from there on first ask and held for the rest of the library's life (a
// key read once, not at every operation written).
class PersonFile {
public:
    explicit PersonFile(QString file = Person::userKeyFile()) : m_file(std::move(file)) {}

    [[nodiscard]] const QString &file() const noexcept { return m_file; }
    // The person kept in the file, made there on first use. A key kept where
    // the first builds of 0.1.7 kept it, beside the library in `libraryRoot`,
    // is moved here once when there is none here yet. Nothing when there can
    // be no person (the file is not the owner's alone, or not a key): what is
    // written is then unsigned, as before there were people, and why is in the log.
    [[nodiscard]] const Person *get(const QString &libraryRoot = {});
    // Why get() found no person, once it was asked.
    [[nodiscard]] const QString &problem() const noexcept { return m_problem; }
    // From now on the person is `person`, kept in the file in place of the
    // one there: what pairing with another of one's own computers does.
    [[nodiscard]] std::expected<void, QString> adopt(Person person);

private:
    QString m_file;
    QString m_problem;
    std::optional<Person> m_person;
    bool m_tried = false;
};
