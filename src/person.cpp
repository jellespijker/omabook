#include "person.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <array>
#include <string_view>
#include <tuple>
#include <utility>

#include "log.h"
#include "secretfile.h"

namespace {

using namespace Qt::StringLiterals;

// The key file: these eight bytes (a name and the format's version), then the
// 32-byte seed the key pair is made from. Nothing else: the public key is
// made again from the seed, so the two can't disagree.
constexpr std::string_view fileMagic{"omapkey\x01", 8};
constexpr std::size_t fileSize = fileMagic.size() + crypto_sign_SEEDBYTES;
using FileBytes = Crypto::SecretBytes<fileSize>;

constexpr std::string_view fingerprintDomain{"omabook person fingerprint v1"};
constexpr std::size_t fingerprintBytes = 16;

} // namespace

QString Person::userKeyFile() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/omabook/person.key"_s; }

QString Person::keyFile(const QString &libraryRoot) { return QDir(libraryRoot).absoluteFilePath(u"omabook-person.key"_s); }

std::expected<Person, QString> Person::generate() {
    if (!Crypto::ready())
        return std::unexpected(u"libsodium could not be made ready: the system gives it no randomness."_s);
    Seed seed;
    randombytes_buf(seed.data(), Seed::size());
    return fromSeed(seed);
}

std::expected<Person, QString> Person::fromSeed(const Seed &seed) {
    if (!Crypto::ready())
        return std::unexpected(u"libsodium could not be made ready: the system gives it no randomness."_s);
    Person person;
    if (crypto_sign_seed_keypair(Crypto::writableUchars(person.m_public), person.m_secret.data(), seed.data()) != 0)
        return std::unexpected(u"The key pair could not be made."_s);
    return person;
}

std::expected<Person, QString> Person::load(const QString &file) {
    FileBytes bytes;
    if (auto read = SecretFile::read(file, bytes.span(), u"an Omabook key file"_s); !read)
        return std::unexpected(read.error());
    if (!std::ranges::equal(bytes.span().first(fileMagic.size()), std::as_bytes(std::span(fileMagic))))
        return std::unexpected(u"%1 is not an Omabook key file of a kind this Omabook knows."_s.arg(file));
    Seed seed;
    std::ranges::copy(bytes.span().subspan(fileMagic.size()), seed.span().begin());
    return fromSeed(seed);
}

std::expected<void, QString> Person::keep(const QString &file, const Seed &seed, Placing placing) {
    FileBytes bytes;
    std::ranges::copy(std::as_bytes(std::span(fileMagic)), bytes.span().begin());
    std::ranges::copy(seed.span(), bytes.span().subspan(fileMagic.size()).begin());
    return SecretFile::keep(file, bytes.span(), placing);
}

std::expected<Person, QString> Person::loadOrCreate(const QString &file) {
    const QFileInfo info(file);
    if (info.exists() || info.isSymLink())
        return load(file);
    if (!Crypto::ready())
        return std::unexpected(u"libsodium could not be made ready: the system gives it no randomness."_s);
    Seed seed;
    randombytes_buf(seed.data(), Seed::size());
    if (auto kept = keep(file, seed, Placing::KeepWhatIsThere); !kept)
        return std::unexpected(kept.error());
    // Read back what is there: ours, or the one another program kept first.
    return load(file);
}

Person::Seed Person::seed() const {
    Seed seed;
    crypto_sign_ed25519_sk_to_seed(seed.data(), m_secret.data());
    return seed;
}

std::expected<void, QString> Person::keepIn(const QString &file) const { return keep(file, seed(), Placing::Replace); }

QByteArray Person::secretForPairing() const {
    const Seed bytes = seed();
    // The Base64 text is the caller's to wipe; the seed itself is wiped here as it goes.
    QByteArray raw(static_cast<qsizetype>(Seed::size()), Qt::Uninitialized);
    std::ranges::copy(bytes.span(), std::as_writable_bytes(std::span(raw.data(), Seed::size())).begin());
    QByteArray secret = raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    sodium_memzero(raw.data(), Seed::size());
    return secret;
}

std::expected<Person, QString> Person::fromPairingSecret(QByteArrayView secret) {
    auto decoded =
        QByteArray::fromBase64Encoding(secret.toByteArray(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    const auto wipe = [&decoded] { sodium_memzero(decoded.decoded.data(), static_cast<std::size_t>(decoded.decoded.size())); };
    if (!decoded || std::cmp_not_equal(decoded.decoded.size(), crypto_sign_SEEDBYTES)) {
        wipe();
        return std::unexpected(u"What was given as a person's key is not one."_s);
    }
    Seed seed;
    std::ranges::copy(Crypto::bytes(decoded.decoded), seed.span().begin());
    wipe();
    return fromSeed(seed);
}

Person::Signature Person::sign(std::span<const std::byte> message) const {
    Signature signature{};
    crypto_sign_detached(Crypto::writableUchars(signature), nullptr, Crypto::uchars(message), message.size(), m_secret.data());
    return signature;
}

std::expected<QByteArray, QString> Person::sealFor(const PublicKey &to, QByteArrayView message) {
    if (!Crypto::ready())
        return std::unexpected(u"libsodium could not be made ready: the system gives it no randomness."_s);
    std::array<std::byte, crypto_box_PUBLICKEYBYTES> box{};
    if (crypto_sign_ed25519_pk_to_curve25519(Crypto::writableUchars(box), Crypto::uchars(to)) != 0)
        return std::unexpected(u"That person's key can't be sealed to."_s);
    QByteArray sealed(message.size() + static_cast<qsizetype>(crypto_box_SEALBYTES), Qt::Uninitialized);
    const auto into = std::as_writable_bytes(std::span(sealed.data(), static_cast<std::size_t>(sealed.size())));
    if (crypto_box_seal(Crypto::writableUchars(into), Crypto::uchars(Crypto::bytes(message)),
                        static_cast<unsigned long long>(message.size()), Crypto::uchars(box))
        != 0)
        return std::unexpected(u"The message could not be sealed."_s);
    return sealed;
}

std::expected<QByteArray, QString> Person::openSealed(QByteArrayView sealed) const {
    if (std::cmp_less(sealed.size(), crypto_box_SEALBYTES))
        return std::unexpected(u"What was sealed is cut short."_s);
    // The person's key pair as X25519, for this once: the secret half wiped as it goes.
    std::array<std::byte, crypto_box_PUBLICKEYBYTES> box{};
    Crypto::SecretBytes<crypto_box_SECRETKEYBYTES> secret;
    if (crypto_sign_ed25519_pk_to_curve25519(Crypto::writableUchars(box), Crypto::uchars(m_public)) != 0
        || crypto_sign_ed25519_sk_to_curve25519(secret.data(), m_secret.data()) != 0)
        return std::unexpected(u"This person's key can't open what is sealed."_s);
    QByteArray message(sealed.size() - static_cast<qsizetype>(crypto_box_SEALBYTES), Qt::Uninitialized);
    const auto into = std::as_writable_bytes(std::span(message.data(), static_cast<std::size_t>(message.size())));
    if (crypto_box_seal_open(Crypto::writableUchars(into), Crypto::uchars(Crypto::bytes(sealed)),
                             static_cast<unsigned long long>(sealed.size()), Crypto::uchars(box), secret.data())
        != 0) {
        sodium_memzero(message.data(), static_cast<std::size_t>(message.size()));
        return std::unexpected(u"This was not sealed for you, or it was changed on the way."_s);
    }
    return message;
}

bool Person::verify(const PublicKey &key, std::span<const std::byte> message, const Signature &signature) {
    return Crypto::ready()
           && crypto_sign_verify_detached(Crypto::uchars(signature), Crypto::uchars(message), message.size(), Crypto::uchars(key)) == 0;
}

QString Person::idOf(const PublicKey &key) {
    return QString::fromLatin1(Crypto::toByteArray(key).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString Person::fingerprintOf(const PublicKey &key) {
    std::array<std::byte, fingerprintBytes> hash{};
    crypto_generichash_state state{};
    crypto_generichash_init(&state, nullptr, 0, hash.size());
    crypto_generichash_update(&state, Crypto::uchars(std::as_bytes(std::span(fingerprintDomain))), fingerprintDomain.size());
    crypto_generichash_update(&state, Crypto::uchars(key), key.size());
    crypto_generichash_final(&state, Crypto::writableUchars(hash), hash.size());
    const QString hex = QString::fromLatin1(Crypto::toByteArray(hash).toHex());
    QStringList groups;
    for (qsizetype at = 0; at < hex.size(); at += 4)
        groups << hex.mid(at, 4);
    return groups.join(u' ');
}

std::expected<Person::PublicKey, QString> Person::publicKeyFrom(QStringView id) {
    const auto decoded =
        QByteArray::fromBase64Encoding(id.trimmed().toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || std::cmp_not_equal(decoded.decoded.size(), crypto_sign_PUBLICKEYBYTES))
        return std::unexpected(u"%1 is not a person's id."_s.arg(id));
    PublicKey key{};
    std::ranges::copy(Crypto::bytes(decoded.decoded), key.begin());
    return key;
}

const Person *PersonFile::get(const QString &libraryRoot) {
    if (m_tried)
        return m_person ? &*m_person : nullptr;
    m_tried = true;
    const QString formerFile = libraryRoot.isEmpty() ? QString() : Person::keyFile(libraryRoot);
    // Kept beside a library by the first builds of 0.1.7: moved here, once,
    // so a library folder that is synced elsewhere no longer carries it.
    if (!formerFile.isEmpty() && !QFileInfo::exists(m_file) && QFileInfo::exists(formerFile)) {
        if (const auto former = Person::load(formerFile); former && Person::keep(m_file, former->seed(), Person::Placing::KeepWhatIsThere))
            std::ignore = QFile::remove(formerFile); // Moved; were it to stay, the one here is what counts.
    }
    auto person = Person::loadOrCreate(m_file);
    if (!person) {
        m_problem = person.error();
        qCWarning(lcLibrary).noquote() << "what this library writes goes unsigned:" << m_problem;
        return nullptr;
    }
    m_person.emplace(std::move(*person));
    return &*m_person;
}

std::expected<void, QString> PersonFile::adopt(Person person) {
    if (auto kept = person.keepIn(m_file); !kept)
        return kept;
    m_person.emplace(std::move(person));
    m_tried = true;
    m_problem.clear();
    return {};
}
