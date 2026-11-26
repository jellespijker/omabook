#include "libraryhold.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSysInfo>
#include <cerrno>
#include <csignal>
#include <tuple>

#include "library.h"
#include "librarybatch.h"
#include "librarymigrations.h"
#include "librarypresence.h"
#include "log.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString holdFile(const QString &root) { return QDir(root).filePath(QString::fromLatin1(LibraryHold::fileName)); }

// Whether a process of this computer is running still; one of another user's is.
[[nodiscard]] bool running(qint64 pid) { return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM; }

} // namespace

std::expected<std::unique_ptr<LibraryHold>, Failure> LibraryHold::take(const QString &root, std::chrono::milliseconds wait) {
    if (!QDir().mkpath(root))
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {root}));
    auto hold = std::make_unique<LibraryHold>(Taking{}, holdFile(root));
    // Held only while its program runs: never given up on for its age, and
    // taken over at once from a program that is gone (a daemon killed).
    hold->m_lock.setStaleLockTime(std::chrono::milliseconds::zero());
    if (hold->m_lock.tryLock(wait))
        return hold;
    if (hold->m_lock.error() == QLockFile::LockFailedError) {
        qint64 pid = 0;
        QString host;
        QString program;
        std::ignore = hold->m_lock.getLockInfo(&pid, &host, &program); // the pid stays 0 when it can't be read
        return std::unexpected(Failure(Failure::Code::LibraryKeptByHub, {root, QString::number(pid)}));
    }
    return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {holdFile(root)}));
}

std::optional<qint64> LibraryHold::heldElsewhere(const QString &root) {
    const QString file = holdFile(root);
    if (!QFileInfo::exists(file))
        return std::nullopt;
    const QLockFile lock(file);
    qint64 pid = 0;
    QString host;
    QString program;
    if (!lock.getLockInfo(&pid, &host, &program) || pid <= 0)
        return std::nullopt;
    const bool here = host.isEmpty() || host == QSysInfo::machineHostName();
    if (here && (pid == QCoreApplication::applicationPid() || !running(pid)))
        return std::nullopt;
    return pid;
}

Library &Library::createIfMissing(bool make) noexcept {
    m_mayCreate = make;
    return *this;
}

// Opening is here, beside the hold, as it is what keeps a hub's libraries
// to the one program that holds them. Its signature is library.h's, as it
// was before it moved; the callers move to std::expected when touched.
bool Library::open(QString *error) { // ast-grep-ignore: prefer-expected-over-error-out-param
    const auto fail = [this, error](const QString &why) {
        qCWarning(lcLibrary).noquote() << m_root << "can't be opened:" << why;
        if (error != nullptr)
            *error = why;
        return false;
    };
    if (const auto holder = LibraryHold::heldElsewhere(m_root))
        return fail(Failure(Failure::Code::LibraryKeptByHub, {m_root, QString::number(*holder)}).text());
    // One expected here and gone (a drive unplugged) is not made anew (#221).
    if (!m_mayCreate && LibraryPresence::missing(m_root))
        return fail(Failure(Failure::Code::LibraryMissing, {m_root}).text());
    if (!QDir().mkpath(m_root))
        return fail("The library folder can't be created: " + m_root);
    QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    database.setDatabaseName(databasePath());
    // The window and a command can write at once; the later one waits, as
    // long as a batch can take (librarybatch.h), and then says so.
    database.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(WriteTurn::longest.count()));
    if (!database.open())
        return fail("The library can't be opened: " + database.lastError().text());
    const auto setUp = LibraryMigrations::setUp(*this);
    if (!setUp)
        return fail(setUp.error());
    LibraryPresence::opened(m_root);
    return true;
}
