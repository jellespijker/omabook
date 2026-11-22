#pragma once

#include <QDateTime>
#include <QString>
#include <cstdint>
#include <expected>
#include <optional>

#include "failure.h"
#include "restic.h"

class Library;

// Why a snapshot is taken (ADR 0014): the nightly one, or a moment snapshot
// just before something that changes much at once, or when the person asks.
enum class SnapshotReason : std::uint8_t { Nightly, Asked, Sync, Import, Tidy, Merge, Restore, Upgrade, BulkChange };

// Where a computer keeps its snapshots: the repository, and the password
// restic opens it with. Chosen once, at setup, and kept in `state`
// (snapshots.json); the password is a 0600 file there, or on a hub one of
// systemd's credentials (omabook-snapshots in $CREDENTIALS_DIRECTORY), which
// is then read first. A destination elsewhere (#182) is another repository.
struct SnapshotPlace {
    QString state;
    ResticRepository repository;

    // This computer's: $XDG_STATE_HOME/omabook/snapshots.
    [[nodiscard]] static SnapshotPlace here();
    // The one kept in `state`, as a test or a hub chooses it.
    [[nodiscard]] static SnapshotPlace in(const QString &state);
    // The repository chosen when none is: $XDG_DATA_HOME/omabook/snapshots.
    [[nodiscard]] static QString defaultRepository();
};

// What of a computer's goes into a library's snapshot besides the library
// folder (the database, copied whole, the books' files and covers): the
// settings, which hold no secrets (those are in the keyring), and the
// plugins' data/ folders (Notebooks' pages and recordings), without the
// plugins themselves, which can be installed again.
struct SnapshotContents {
    QString settingsFile;
    QString pluginsFolder;

    // This computer's: config.json and the plugins' folder.
    [[nodiscard]] static SnapshotContents here();
};

// The snapshots of a computer's libraries, made with restic (ADR 0014): what
// `omabook snapshot` and, on a hub, the daemon (#179) call. A snapshot is
// consistent: the library's database is copied inside its write turn (a
// batch, librarybatch.h), so no writer is half done in it, then restic keeps
// that copy and the files. Call these between writes, on the thread that
// writes, never inside a batch of another connection to the same library.
class Snapshots {
public:
    explicit Snapshots(SnapshotPlace place, SnapshotContents contents = SnapshotContents::here());

    // Whether snapshots are set up here: a repository chosen, with its password.
    [[nodiscard]] bool ready() const;
    // Makes the repository at `repository` (the default one when empty) and
    // its password, and gives the password back as the recovery key: to show
    // once, for the person to keep, since nothing is restored without it.
    [[nodiscard]] std::expected<QString, Failure> setUp(const QString &repository = {});

    struct Taken {
        QString id;             // the snapshot's, or the last one's when nothing changed
        bool unchanged = false; // nothing changed since the last one, so none was taken
    };
    // A snapshot of `library` now, for `reason`. A moment snapshot only when
    // something changed since the library's last one; a nightly one always.
    [[nodiscard]] std::expected<Taken, Failure> take(const Library &library, SnapshotReason reason) const;
    // A moment snapshot just before something risky, when snapshots are set
    // up here; nothing (no Taken) when they aren't.
    [[nodiscard]] std::expected<std::optional<Taken>, Failure> before(const Library &library, SnapshotReason moment) const;
    // The nightly snapshot when one is due at `now`, and then the thinning;
    // nothing when none is due or snapshots aren't set up.
    [[nodiscard]] std::expected<std::optional<Taken>, Failure> nightly(const Library &library, const QDateTime &now) const;

    // The library's snapshots, oldest first.
    [[nodiscard]] std::expected<QList<Restic::Snapshot>, Failure> list(const Library &library) const;
    // Forgets the library's snapshots that are kept no longer at `now`
    // (SnapshotRetention), and says how many.
    [[nodiscard]] std::expected<qsizetype, Failure> thin(const Library &library, const QDateTime &now) const;

    // The tags of a snapshot: "omabook", "library:<key>", "nightly" or
    // "moment", "reason:<why>" and "state:<what it held>".
    [[nodiscard]] static QString reasonName(SnapshotReason reason);
    [[nodiscard]] static QString libraryTag(const Library &library);

private:
    SnapshotPlace m_place;
    SnapshotContents m_contents;
};
