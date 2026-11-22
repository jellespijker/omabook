#pragma once

#include <QLockFile>
#include <QString>
#include <QtTypes>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>

#include "failure.h"

// A hub's hold on a library it keeps (ADR 0013): on a hub the daemon alone
// writes its libraries, and every other program asks it to. It holds each one
// by a lock file in the library's folder for as long as it runs, and a
// library is opened (Library::open, which is defined beside this) only by the
// program that holds it, or by any program when none does: on a person's
// computer, where no hub runs, nothing changes. A daemon that was killed
// leaves the file behind, and its pid is then gone: that hold is no one's,
// and the daemon that comes back takes it at once.
class LibraryHold {
    // Only take() makes one, holding the file by then.
    struct Taking {
        explicit Taking() = default;
    };

public:
    // The file in a library's folder that says who holds it.
    static constexpr auto fileName = ".omabook-hub.lock";

    // Takes the hold on the library in `root` for this program, waiting `wait`
    // at most for another program that holds it: another hub on the same folder.
    [[nodiscard]] static std::expected<std::unique_ptr<LibraryHold>, Failure>
    take(const QString &root, std::chrono::milliseconds wait = std::chrono::seconds(5));

    // The process of another program, running still, that holds the library
    // in `root`; none when nothing holds it, or this program does.
    [[nodiscard]] static std::optional<qint64> heldElsewhere(const QString &root);

    LibraryHold([[maybe_unused]] Taking taking, const QString &file) : m_lock(file) {}
    ~LibraryHold() = default;
    LibraryHold(const LibraryHold &) = delete;
    LibraryHold &operator=(const LibraryHold &) = delete;
    LibraryHold(LibraryHold &&) = delete;
    LibraryHold &operator=(LibraryHold &&) = delete;

private:
    QLockFile m_lock;
};
