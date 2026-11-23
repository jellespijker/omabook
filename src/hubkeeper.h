#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "failure.h"

class Library;
class LibraryHold;

// The hub's one writer (ADR 0013): it holds and opens every library in the
// hub's folder of libraries, for as long as the daemon runs, and is the only
// part of a hub that does. What writes there (a computer's session, the hub's
// own jobs, a snapshot) is handed a library by it, one at a time: while one
// has it, the others wait, so no two writes on a hub ever meet in SQLite's lock
// (#161), and a snapshot always sees a whole library.
//
// The libraries are opened on the thread that opens the keeper (SQLite's
// connections are per thread): the daemon's. Its lock is taken from any
// thread, but the work done in it runs on the caller's: the daemon's again,
// for what it is asked over its socket and for its schedule.
class HubKeeper {
public:
    // A write that changes much at once, so that a moment snapshot (ADR 0014,
    // #181) is taken just before it, inside the same lock.
    enum class Moment : std::uint8_t { None, Sync, Import, Tidy, Merge, Restore, Upgrade, Bulk, Asked };
    [[nodiscard]] static QString momentName(Moment moment);

    // What is done with one library: its answer, or why not.
    using Work = std::function<std::expected<QJsonObject, Failure>(Library &library)>;
    // What is done with all of them at once.
    using Whole = std::function<std::expected<void, Failure>(std::span<Library *const> libraries)>;
    // What runs before every write of a Moment other than None, inside its
    // lock: refusing it refuses the write, which then changes nothing.
    using Before = std::function<std::expected<void, Failure>(Library &library, Moment moment)>;

    // The libraries in `folder`: each folder in it that holds an Omabook library.
    explicit HubKeeper(QString folder);
    ~HubKeeper();
    HubKeeper(const HubKeeper &) = delete;
    HubKeeper &operator=(const HubKeeper &) = delete;
    HubKeeper(HubKeeper &&) = delete;
    HubKeeper &operator=(HubKeeper &&) = delete;

    // Holds and opens each library there, refused when another hub holds one
    // (or it can't be opened). Done once, before anything asks.
    [[nodiscard]] std::expected<void, Failure> open();
    [[nodiscard]] const QString &folder() const noexcept { return m_folder; }
    // The names of the libraries it keeps (their folders' names), sorted.
    [[nodiscard]] QStringList names() const;

    // Adds what is done before each write that changes much: #181 takes its
    // moment snapshot here. Added before open(), as opening may upgrade a
    // library, which is such a write.
    void before(Before before) { m_before.push_back(std::move(before)); }

    // `work` on the library called `name` (any, when it keeps only one and
    // `name` is empty), alone: nothing else of the hub writes meanwhile.
    [[nodiscard]] std::expected<QJsonObject, Failure> write(const QString &name, Moment moment, const Work &work);
    // `work` with every library, alone: what has to see all of them as they
    // stand at one moment (the nightly snapshot, the hub's status).
    [[nodiscard]] std::expected<void, Failure> whole(const Whole &work);
    // A copy of the library called `name` as it stands now, its database at
    // `folder` (made anew), to work on beside it while the others write: the
    // hub's background work (#192) runs there, then hands its result back
    // through write(). Not opened, so that the thread working on it opens it.
    [[nodiscard]] std::expected<std::unique_ptr<Library>, Failure> copy(const QString &name, const QString &folder);
    // Whether something has the libraries now; while it does, the others wait.
    [[nodiscard]] bool busy() const;

private:
    struct Kept;
    // The lock, taken once: work inside work can only wait for itself, and is refused.
    class Turn;

    [[nodiscard]] std::expected<Library *, Failure> find(const QString &name) const;

    QString m_folder;
    std::vector<std::unique_ptr<Kept>> m_kept;
    std::vector<Before> m_before;
    std::mutex m_lock;
    std::thread::id m_holder; // whose the lock is, guarded by m_holding
    mutable std::mutex m_holding;
};
