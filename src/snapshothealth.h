#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <utility>

#include "failure.h"
#include "restic.h"
#include "snapshots.h"
#include "snapshotwarnings.h"

// How a computer's snapshots stand (#184): what is known of each repository
// they are kept in, kept in their state folder (health.json) so that the
// warnings are worked out at any moment without asking restic, and an absent
// disk is still known by what it last held. Filled in by what is done with
// the snapshots: the nightly one (noteSnapshot), the checks (check), and a
// look at what each repository holds (look). Each call reads the record and
// writes it back, so the hub's daemon and a command can take turns at it.
class SnapshotHealth {
public:
    // One repository of snapshots, as the checks reach it: the computer's own,
    // or a destination's (#182), with what its own record says of it.
    struct Target {
        QString name;
        SnapshotWarnings::Medium medium = SnapshotWarnings::Medium::Own;
        QString location;
        std::expected<ResticRepository, Failure> repository; // or why it can't be opened
        QString passwordAt;                                  // where its password belongs, said when it is missing
        bool absent = false;                                 // a second disk that isn't there
        std::optional<SnapshotWarnings::Outcome> snapshot;   // a destination's last copy, as it keeps it
    };
    // The destinations' targets, read afresh each time.
    using Targets = std::function<QList<Target>()>;
    // Those of the destinations kept beside the snapshots of `place` (#182):
    // each a second disk or a place away, opened with its own password, absent
    // when its disk isn't there, with how its last copy went.
    [[nodiscard]] static Targets destinationsOf(const SnapshotPlace &place);

    enum class Depth : std::uint8_t {
        Light, // restic check: the repository's structure (weekly)
        Data,  // and its data read back (monthly): all of a disk's, a twelfth of a place away's each month
    };

    // It keeps its own copy of `snapshots`: what they are kept in, not what
    // they hold. Their destinations are those kept beside them unless others are given.
    explicit SnapshotHealth(Snapshots snapshots, Targets destinations = {});

    // Whether there is anything to look after: snapshots chosen a place here.
    [[nodiscard]] bool chosen() const;
    // The computer's own repository, then each destination's.
    [[nodiscard]] QList<Target> targets() const;

    // How the nightly snapshot into the computer's own repository went.
    void noteSnapshot(const std::expected<void, Failure> &outcome, const QDateTime &now) const;
    // restic's check of each repository that can be reached, kept; a missing
    // password or an absent disk is no failed check, and has its own warning.
    void check(Depth depth, const QDateTime &now) const;
    // What each repository holds now (its newest snapshot of each library),
    // whether it can be read, and the room left on its disk.
    void look(const QDateTime &now) const;

    // What is known of each repository, the record joined with its target.
    [[nodiscard]] QList<SnapshotWarnings::Repository> repositories() const;
    // The warnings at `now` for these libraries (tag and name), each first seen kept.
    [[nodiscard]] QList<SnapshotWarnings::Warning> warnings(const QList<std::pair<QString, QString>> &libraries, const QDateTime &now,
                                                            const SnapshotWarnings::Thresholds &thresholds = {}) const;
    // Of `warnings`, the ones to tell the person now: new ones, and ones told
    // `remindAfter` ago that still stand; kept as told. One that went away is
    // forgotten, so it is told again should it come back. Never nags.
    [[nodiscard]] QList<SnapshotWarnings::Warning> toTell(const QList<SnapshotWarnings::Warning> &warnings, const QDateTime &now,
                                                          std::chrono::days remindAfter = SnapshotWarnings::Thresholds{}.remindAfter) const;

    // The repositories and the warnings, for `snapshot status --json` and the hub's status.
    [[nodiscard]] QJsonObject json(const QList<SnapshotWarnings::Warning> &warnings) const;

    // What restic reads back of a repository in a data check at `now`: all of
    // it on a disk here, and the month's twelfth of a place away, so a year reads it all.
    [[nodiscard]] static QString readDataOf(SnapshotWarnings::Medium medium, const QDateTime &now);

private:
    [[nodiscard]] QJsonObject read() const;
    void write(const QJsonObject &record) const;

    Snapshots m_snapshots;
    Targets m_destinations;
    QString m_file;
};
