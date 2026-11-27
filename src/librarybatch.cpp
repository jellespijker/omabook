#include "librarybatch.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <thread>

#include "changelog.h"
#include "library.h"
#include "log.h"
#include "trash.h"

using namespace Qt::StringLiterals;

// The writers of one database file in this program, in the order they asked:
// the first one's turn it is.
struct WriteLine {
    struct Writer {
        std::thread::id thread;
        QString connection;
    };
    std::mutex mutex;
    std::condition_variable turned;
    std::list<Writer> waiting;
    int taken = 0; // how often the first took its turn, a batch inside a batch
};

namespace {

// The line of the database at `file`, kept while someone is in it.
[[nodiscard]] std::shared_ptr<WriteLine> lineOf(const QString &file) {
    static std::mutex mutex;
    static std::map<QString, std::weak_ptr<WriteLine>> lines;
    const std::scoped_lock lock(mutex);
    std::erase_if(lines, [](const auto &line) { return line.second.expired(); });
    std::weak_ptr<WriteLine> &kept = lines[file];
    auto line = kept.lock();
    if (!line) {
        line = std::make_shared<WriteLine>();
        kept = line;
    }
    return line;
}

} // namespace

bool readOnly(const QString &file) {
    const std::array paths{QFileInfo(file).absolutePath(), file, file + u"-wal"_s, file + u"-journal"_s};
    return std::ranges::any_of(paths, [](const QString &path) {
        const QFileInfo info(path);
        return info.exists() && !info.isWritable();
    });
}

bool diskFull(const QString &file) {
    // Less than the log's index and a few pages beside the database take.
    constexpr qint64 least = 64LL * 1024;
    const QStorageInfo disk(QFileInfo(file).absolutePath());
    return disk.isValid() && disk.bytesAvailable() >= 0 && disk.bytesAvailable() < least;
}

std::optional<Failure> plainlyUnwritable(const QString &file) {
    if (!QFileInfo::exists(file))
        return Failure(Failure::Code::LibraryMissing, {QFileInfo(file).absolutePath()});
    if (readOnly(file))
        return Failure(Failure::Code::LibraryReadOnly, {QFileInfo(file).absolutePath()});
    if (diskFull(file))
        return Failure(Failure::Code::LibraryDiskFull, {QFileInfo(file).absolutePath()});
    return std::nullopt;
}

Failure whyNotWritten(const QString &file) { return plainlyUnwritable(file).value_or(Failure(Failure::Code::LibraryBusy)); }

Failure notWritten(const QString &file) { return plainlyUnwritable(file).value_or(Failure(Failure::Code::LibraryNotWritten)); }

std::expected<void, Failure> trashRemoved(const QString &database, const QString &root, const QString &folder) {
    if (const auto why = plainlyUnwritable(database))
        return std::unexpected(*why);
    return Trash::folder(root, folder);
}

bool makeBookFolder(const QString &database, const QString &folder) { return QFileInfo::exists(database) && QDir().mkpath(folder); }

WriteTurn::WriteTurn(const QString &file, const QString &connection, std::chrono::milliseconds wait) : m_line(lineOf(file)) {
    std::unique_lock lock(m_line->mutex);
    std::list<WriteLine::Writer> &waiting = m_line->waiting;
    const auto thread = std::this_thread::get_id();
    if (!waiting.empty() && waiting.front().thread == thread) {
        m_had = waiting.front().connection == connection;
        if (m_had)
            ++m_line->taken;
        else
            qCWarning(lcLibrary).noquote() << file << "is written through two connections at once on one thread; the second"
                                           << "could only wait for the first, so it is refused.";
        return;
    }
    const auto mine = waiting.insert(waiting.end(), {.thread = thread, .connection = connection});
    m_had = m_line->turned.wait_for(lock, wait, [&waiting, mine] { return waiting.begin() == mine; });
    if (m_had) {
        m_line->taken = 1;
        return;
    }
    waiting.erase(mine); // never the first: that one has its turn
    qCWarning(lcLibrary).noquote() << file << "was written by another connection here for longer than"
                                   << std::chrono::duration<double>(wait).count() << "seconds; this write is given up.";
}

WriteTurn::~WriteTurn() {
    if (!m_had)
        return;
    const std::scoped_lock lock(m_line->mutex);
    if (--m_line->taken > 0)
        return;
    m_line->waiting.pop_front();
    m_line->turned.notify_all();
}

Library::Batch::Batch(const Library &library, Lock lock) : m_library(library) {
    if (m_library.m_batches++ > 0)
        return;
    m_turn = std::make_unique<WriteTurn>(m_library.databasePath(), m_library.m_connection);
    // Nor is one that went away written, by way of the files still open (#221).
    if (!QFileInfo::exists(m_library.databasePath()) || !m_turn->had()) {
        m_begun = false;
        m_turn.reset();
        return;
    }
    QSqlDatabase database = QSqlDatabase::database(m_library.m_connection);
    QSqlQuery begin(database);
    m_begun = lock == Lock::AtOnce ? begin.exec("BEGIN IMMEDIATE") : database.transaction();
    if (m_begun)
        return;
    // Another program at it still after the wait, or a library that can only be read.
    qCWarning(lcLibrary).noquote() << m_library.databasePath()
                                   << "can't be written now:" << (lock == Lock::AtOnce ? begin.lastError() : database.lastError()).text();
    m_turn.reset();
}

Library::Batch::~Batch() {
    if (m_library.m_batches == 1)
        ChangeLog::afterWrite(m_library);
    if (--m_library.m_batches == 0)
        QSqlDatabase::database(m_library.m_connection).commit();
    // The turn goes with the batch, after its commit.
}
