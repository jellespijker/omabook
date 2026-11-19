#include "person.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>

#include "log.h"

namespace {

using namespace Qt::StringLiterals;

// The key file: these eight bytes (a name and the format's version), then the
// 32-byte seed the key pair is made from. Nothing else: the public key is
// made again from the seed, so the two can't disagree.
constexpr std::string_view fileMagic{"omapkey\x01", 8};
constexpr std::size_t fileSize = fileMagic.size() + crypto_sign_SEEDBYTES;
using FileBytes = Crypto::SecretBytes<fileSize>;
// Read and write for its owner (0600); anything for the group or others (0077).
constexpr mode_t ownerOnly = 0600U;
constexpr mode_t groupOrOthers = 0077U;

constexpr std::string_view fingerprintDomain{"omabook person fingerprint v1"};
constexpr std::size_t fingerprintBytes = 16;

// A file descriptor, closed when it goes.
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept : m_fd(fd) {}
    ~FileDescriptor() {
        // Nothing can be done about a failed close on a path that is already failing.
        if (m_fd >= 0)
            std::ignore = ::close(m_fd);
    }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept {
        std::swap(m_fd, other.m_fd);
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }
    // Closes it now, answering whether everything written reached the file.
    [[nodiscard]] bool close() noexcept {
        const int fd = std::exchange(m_fd, -1);
        return fd < 0 || ::close(fd) == 0;
    }

private:
    int m_fd = -1;
};

[[nodiscard]] QString failure(const QString &what, const QString &file) { return u"%1 %2: %3"_s.arg(what, file, qt_error_string(errno)); }

[[nodiscard]] bool readAll(int fd, std::span<std::byte> into) {
    while (!into.empty()) {
        const ssize_t got = ::read(fd, into.data(), into.size());
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return false;
        into = into.subspan(static_cast<std::size_t>(got));
    }
    return true;
}

[[nodiscard]] bool writeAll(int fd, std::span<const std::byte> from) {
    while (!from.empty()) {
        const ssize_t put = ::write(fd, from.data(), from.size());
        if (put < 0 && errno == EINTR)
            continue;
        if (put <= 0)
            return false;
        from = from.subspan(static_cast<std::size_t>(put));
    }
    return true;
}

// Puts the finished temporary file in place without ever replacing a key
// that is there: when another program kept one first, that one stays.
// The temporary file is gone afterwards, whichever way it went.
[[nodiscard]] bool placeWithoutReplacing(const QByteArray &from, const QByteArray &to) {
    if (::renameat2(AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) == 0)
        return true;
    bool placed = errno == EEXIST;
    // A file system without renameat2's flags: a hard link is as atomic and
    // as unwilling to replace.
    if (errno == EINVAL || errno == ENOSYS)
        placed = ::link(from.constData(), to.constData()) == 0 || errno == EEXIST;
    const int why = errno;
    std::ignore = ::unlink(from.constData()); // Only the temporary name goes; a key stays under its own.
    errno = why;
    return placed;
}

// Puts the finished temporary file in place of whatever key is there.
[[nodiscard]] bool placeReplacing(const QByteArray &from, const QByteArray &to) {
    if (::rename(from.constData(), to.constData()) == 0)
        return true;
    const int why = errno;
    std::ignore = ::unlink(from.constData()); // Only the temporary name goes; the key there stays as it was.
    errno = why;
    return false;
}

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
    const QByteArray path = QFile::encodeName(file);
    const FileDescriptor fd{::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    if (!fd.valid())
        return std::unexpected(failure(u"Can't read the key file"_s, file));
    struct stat info{};
    if (::fstat(fd.get(), &info) != 0)
        return std::unexpected(failure(u"Can't read the key file"_s, file));
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid())
        return std::unexpected(u"The key file %1 is not a file of yours."_s.arg(file));
    if ((info.st_mode & groupOrOthers) != 0)
        return std::unexpected(u"The key file %1 can be read by others; only you may (chmod 600 it)."_s.arg(file));
    if (std::cmp_not_equal(info.st_size, fileSize))
        return std::unexpected(u"%1 is not an Omabook key file."_s.arg(file));
    FileBytes bytes;
    if (!readAll(fd.get(), bytes.span()))
        return std::unexpected(failure(u"Can't read the key file"_s, file));
    if (!std::ranges::equal(bytes.span().first(fileMagic.size()), std::as_bytes(std::span(fileMagic))))
        return std::unexpected(u"%1 is not an Omabook key file of a kind this Omabook knows."_s.arg(file));
    Seed seed;
    std::ranges::copy(bytes.span().subspan(fileMagic.size()), seed.span().begin());
    return fromSeed(seed);
}

std::expected<void, QString> Person::keep(const QString &file, const Seed &seed, Placing placing) {
    const QFileInfo where(file);
    if (!QDir().mkpath(where.absolutePath()))
        return std::unexpected(u"Can't make the folder %1."_s.arg(where.absolutePath()));
    // mkstemp makes the file for its owner only (0600), before a byte of the
    // key is in it.
    QByteArray temporary = QFile::encodeName(file + u".XXXXXX"_s);
    FileDescriptor fd{::mkstemp(temporary.data())};
    if (!fd.valid())
        return std::unexpected(failure(u"Can't write the key file"_s, file));
    const auto removeTemporary = [&temporary] { std::ignore = ::unlink(temporary.constData()); };
    FileBytes bytes;
    std::ranges::copy(std::as_bytes(std::span(fileMagic)), bytes.span().begin());
    std::ranges::copy(seed.span(), bytes.span().subspan(fileMagic.size()).begin());
    const bool written = ::fchmod(fd.get(), ownerOnly) == 0 && writeAll(fd.get(), bytes.span()) && ::fsync(fd.get()) == 0 && fd.close();
    if (!written) {
        const QString why = failure(u"Can't write the key file"_s, file);
        removeTemporary();
        return std::unexpected(why);
    }
    const bool placed = placing == Placing::Replace ? placeReplacing(temporary, QFile::encodeName(file))
                                                    : placeWithoutReplacing(temporary, QFile::encodeName(file));
    if (!placed)
        return std::unexpected(failure(u"Can't keep the key file"_s, file));
    // The new name is only lasting once the folder that holds it is written.
    if (const FileDescriptor folder{::open(QFile::encodeName(where.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        folder.valid())
        std::ignore = ::fsync(folder.get()); // The key is in place either way; this only hurries it to the disk.
    return {};
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
