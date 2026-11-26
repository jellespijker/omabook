#include "addinghold.h"

#include <QDir>
#include <QFileInfo>
#include <map>

#include "library.h"
#include "librarybatch.h"

using namespace Qt::StringLiterals;

std::expected<void, Failure> AddingHold::take(const QString &file, std::chrono::milliseconds wait) {
    // The locks this thread holds: one it holds already is shared, as it
    // would otherwise wait for itself.
    thread_local std::map<QString, std::weak_ptr<QLockFile>> held;
    std::erase_if(held, [](const auto &one) { return one.second.expired(); });
    std::weak_ptr<QLockFile> &kept = held[file];
    auto lock = kept.lock();
    if (!lock) {
        lock = std::make_shared<QLockFile>(file);
        // A holder that died is seen to be gone at once. By age, only a lock
        // another computer left in a shared folder is ever given up.
        lock->setStaleLockTime(std::chrono::hours(1));
        if (!lock->tryLock(wait)) {
            if (lock->error() == QLockFile::LockFailedError)
                return std::unexpected(Failure(Failure::Code::LibraryBusy));
            // A library that went away is said to be missing (#221), not unwritable.
            const Failure gone = whyNotWritten(Library::databaseIn(QFileInfo(file).absolutePath()));
            return std::unexpected(gone.code() == Failure::Code::LibraryMissing ? gone : Failure(Failure::Code::LibraryUnwritable, {file}));
        }
        kept = lock;
    }
    m_locks.push_back(std::move(lock));
    return {};
}

std::expected<AddingHold, Failure> AddingHold::files(const QString &root, QStringList hashes, std::chrono::milliseconds wait) {
    // Taken in one order by everyone, so two with files in common don't each
    // hold one the other waits for.
    hashes.sort();
    hashes.removeDuplicates();
    AddingHold hold;
    for (const QString &hash : std::as_const(hashes))
        if (!hash.isEmpty())
            if (const auto taken = hold.take(QDir(root).filePath(u".adding-%1.lock"_s.arg(hash)), wait); !taken)
                return std::unexpected(taken.error());
    return hold;
}

std::expected<AddingHold, Failure> AddingHold::book(const QString &root, qint64 id, std::chrono::milliseconds wait) {
    AddingHold hold;
    if (const auto taken = hold.take(QDir(root).filePath(u".adding-book-%1.lock"_s.arg(id)), wait); !taken)
        return std::unexpected(taken.error());
    return hold;
}
