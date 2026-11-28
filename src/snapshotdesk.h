#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "snapshots.h"

class Library;

// The snapshots of the library in use as the window shows them (#185, ADR
// 0014): where they are kept and the newest in each place, taking one now,
// the list of them, the library diff from one to another or to now, putting a
// book's details or annotations back after a moment snapshot (and undoing
// that), the whole library restored into a new folder, and the destinations
// (#182). What restic is asked runs on a worker, one thing at a time, with a
// library of its own; what comes back is the JSON the command line prints
// (SnapshotRestore), as QML reads it.
class SnapshotDesk : public QObject {
    Q_OBJECT
    // Whether snapshots are set up on this computer, and where they are kept.
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QString repository READ repository NOTIFY changed)
    // That they were set up and their password is gone, with what to do; empty otherwise (#296).
    Q_PROPERTY(QString passwordMissing READ passwordMissing NOTIFY changed)
    // The library's snapshots, newest first: each {"id", "time", "kind" ("nightly" or "moment"), "reason"}.
    Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY changed)
    // Whether they have been listed since the panel opened; false while they never were.
    Q_PROPERTY(bool listed READ listed NOTIFY changed)
    // The folders of other libraries whose snapshots are kept there, listed when this library has none (#292).
    Q_PROPERTY(QStringList otherLibraries READ otherLibraries NOTIFY changed)
    // Each destination: {"name", "location", "kind" ("disk" or "away"), "credentials", "copied",
    // "last": {"tried", "copied", "absent", "said", "latest_id", "latest"}} (the last copy's, when there was one).
    Q_PROPERTY(QVariantList destinations READ destinations NOTIFY changed)
    // Three copies on two kinds of medium, one away from home.
    Q_PROPERTY(bool backedUp READ backedUp NOTIFY changed)
    // What is wrong with the snapshots, each saying what to do (#184).
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed)
    // The last diff asked for: SnapshotRestore::Compared::toJson; empty before one.
    Q_PROPERTY(QVariantMap diff READ diff NOTIFY changed)
    // What putting a book back would change, from the library now to the
    // snapshot: {"snapshot", "book" (LibraryDiff::BookChange::toJson)}; empty when none is asked.
    Q_PROPERTY(QVariantMap putBack READ putBack NOTIFY changed)
    // The last book put back: {"snapshot", "book", "title", "details", "annotations", "undo",
    // "restored": {"details", "annotations", "strokes"}}; empty before one.
    Q_PROPERTY(QVariantMap restored READ restored NOTIFY changed)
    // The folder the whole library was last restored into.
    Q_PROPERTY(QString restoredInto READ restoredInto NOTIFY changed)
    // A destination's test: {"name", "snapshots", "latest" (a snapshot, or none)}.
    Q_PROPERTY(QVariantMap tested READ tested NOTIFY changed)
    // A recovery key just made, to show once: {"for" ("" for this computer's, else the destination's name), "key"}.
    Q_PROPERTY(QVariantMap recoveryKey READ recoveryKey NOTIFY changed)
    // What is being done ("list", "setup", "take", "diff", "preview", "restore", "whole", "add",
    // "test", "remove"), empty when nothing is; and how the last of them went.
    Q_PROPERTY(QString doing READ doing NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool failed READ failed NOTIFY changed)

public:
    // The folder of the library in use.
    using Root = std::function<QString()>;
    // What is wrong with the snapshots of a library, each worded with what to do; run on the worker.
    using Warnings = std::function<QStringList(const Snapshots &snapshots, const Library &library)>;
    // The warnings as the checks (#184, SnapshotHealth) left them, each worded with what to do: what
    // `omabook snapshot status` says, worked out from what was last known, without asking restic.
    [[nodiscard]] static Warnings health();

    // `place` is a computer's (SnapshotPlace::here, or a test's in()): read again from its state folder each
    // time. The warnings are the checks' unless others are given.
    SnapshotDesk(SnapshotPlace place, SnapshotContents contents, Root root, Warnings warnings = health(), QObject *parent = nullptr);

    [[nodiscard]] bool ready() const;
    [[nodiscard]] QString repository() const;
    [[nodiscard]] QString passwordMissing() const;
    [[nodiscard]] QVariantList snapshots() const { return m_snapshots; }
    [[nodiscard]] bool listed() const noexcept { return m_listed; }
    [[nodiscard]] QStringList otherLibraries() const { return m_otherLibraries; }
    [[nodiscard]] QVariantList destinations() const { return m_destinations; }
    [[nodiscard]] bool backedUp() const noexcept { return m_backedUp; }
    [[nodiscard]] QStringList warnings() const { return m_warnings; }
    [[nodiscard]] QVariantMap diff() const { return m_diff; }
    [[nodiscard]] QVariantMap putBack() const { return m_putBack; }
    [[nodiscard]] QVariantMap restored() const { return m_restored; }
    [[nodiscard]] QString restoredInto() const { return m_restoredInto; }
    [[nodiscard]] QVariantMap tested() const { return m_tested; }
    [[nodiscard]] QVariantMap recoveryKey() const { return m_recoveryKey; }
    [[nodiscard]] QString doing() const { return m_doing; }
    [[nodiscard]] QString status() const { return m_status; }
    [[nodiscard]] bool failed() const noexcept { return m_failed; }

    // The snapshots and destinations listed again, and the warnings.
    Q_INVOKABLE void refresh();
    // Sets snapshots up, in `repository` or the usual place when empty; the recovery key is shown once.
    // With `recoveryKey`, opens the snapshots already kept there instead (#292).
    Q_INVOKABLE void setUp(const QString &repository, const QString &recoveryKey = {});
    // A snapshot of the library now, when something changed since the last.
    Q_INVOKABLE void takeNow();
    // The library diff from the snapshot `from` to `to`, or to now when `to` is empty.
    Q_INVOKABLE void compare(const QString &from, const QString &to);
    // What putting the book of `book` (its global id) back as `snapshot` has it would change.
    Q_INVOKABLE void preview(const QString &snapshot, const QString &book);
    // Puts it back: its details, its annotations, or both, after a moment snapshot to undo it with.
    Q_INVOKABLE void restoreBook(const QString &snapshot, const QString &book, bool details, bool annotations);
    // The last book put back goes back to how it was, from the moment snapshot taken before.
    Q_INVOKABLE void undoRestore();
    // The whole library as `snapshot` holds it, into `folder`, which is new or empty.
    Q_INVOKABLE void restoreWhole(const QString &snapshot, const QString &folder);
    // A destination added: a second disk's folder, or a place restic reaches; each credential by
    // its name, with its value. Its recovery key is shown once.
    Q_INVOKABLE void addDestination(const QString &name, const QString &location, const QStringList &credentialNames,
                                    const QStringList &credentialValues);
    Q_INVOKABLE void testDestination(const QString &name);
    // Forgets the destination; what is kept there stays.
    Q_INVOKABLE void removeDestination(const QString &name);
    // What is shown once, gone: the recovery key, the preview, the test's answer.
    Q_INVOKABLE void forget();

signals:
    void changed();
    // A book was put back into the library in use.
    void booksChanged();

private:
    // How a job went, in words, and what it found, kept on the window's thread by `keep`.
    struct Outcome {
        QString said;
        bool failed = false;
        std::function<void(SnapshotDesk &desk)> keep;
    };
    // The snapshots and destinations as a job left them.
    struct Listing;
    void work(const QString &doing, std::function<Outcome()> job);
    [[nodiscard]] Listing listing(const Snapshots &snapshots, const Library *library) const;
    void keep(const Listing &listing);
    // The snapshots as they are kept now: the place read again, since setting them up chooses its repository.
    [[nodiscard]] Snapshots made() const { return Snapshots(SnapshotPlace::in(m_place.state), m_contents); }

    SnapshotPlace m_place;
    SnapshotContents m_contents;
    Root m_root;
    Warnings m_warn;
    QVariantList m_snapshots, m_destinations;
    bool m_listed = false, m_backedUp = false, m_failed = false;
    QStringList m_warnings, m_otherLibraries;
    QVariantMap m_diff, m_putBack, m_restored, m_tested, m_recoveryKey;
    QString m_restoredInto, m_doing, m_status;
};
