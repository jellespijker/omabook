#include "secretfile.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace {

using namespace Qt::StringLiterals;

// Read and write for its owner (0600); anything for the group or others (0077).
constexpr mode_t ownerOnly = 0600U;
constexpr mode_t groupOrOthers = 0077U;

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

std::expected<void, QString> SecretFile::keep(const QString &file, std::span<const std::byte> bytes, Placing placing) {
    const QFileInfo where(file);
    if (!QDir().mkpath(where.absolutePath()))
        return std::unexpected(u"Can't make the folder %1."_s.arg(where.absolutePath()));
    // mkstemp makes the file for its owner only (0600), before a byte of the
    // key is in it.
    QByteArray temporary = QFile::encodeName(file + u".XXXXXX"_s);
    FileDescriptor fd{::mkstemp(temporary.data())};
    if (!fd.valid())
        return std::unexpected(failure(u"Can't write the key file"_s, file));
    const bool written = ::fchmod(fd.get(), ownerOnly) == 0 && writeAll(fd.get(), bytes) && ::fsync(fd.get()) == 0 && fd.close();
    if (!written) {
        const QString why = failure(u"Can't write the key file"_s, file);
        std::ignore = ::unlink(temporary.constData()); // The half-written temporary goes; the error says why.
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

std::expected<void, QString> SecretFile::read(const QString &file, std::span<std::byte> into, const QString &kind) {
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
    if (std::cmp_not_equal(info.st_size, into.size()))
        return std::unexpected(u"%1 is not %2."_s.arg(file, kind));
    if (!readAll(fd.get(), into))
        return std::unexpected(failure(u"Can't read the key file"_s, file));
    return {};
}
