#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTypes>
#include <chrono>
#include <cstdint>
#include <map>

#include "failure.h"

// When a computer's snapshots are no longer what they should be (#184, ADR
// 0014): a backup is never old unnoticed. Worked out from what is known of
// each repository the snapshots are kept in (the computer's own, and each
// destination's) at a moment given, so a test sets the clock. Each warning is
// a Failure (ADR 0005): worded where it is shown, and saying what to do.
namespace SnapshotWarnings {

// What kind of repository: the computer's own, a second disk, or a place away from home.
enum class Medium : std::uint8_t { Own, Disk, Away };

// How something last went: never, when `ran` isn't valid.
struct Outcome {
    QDateTime ran;
    bool done = false;
    QString said; // why not, in English
};

// What is known of one repository of snapshots.
struct Repository {
    QString name; // empty for the computer's own; else the destination's
    Medium medium = Medium::Own;
    QString location;   // restic's --repo
    QString passwordAt; // where its password should be and isn't; empty when it is there
    bool absent = false;
    QString unreadable;                  // what restic said when it couldn't be listed; empty when it could
    QDateTime known;                     // when it was first looked at: since when a check is due
    qint64 free = -1;                    // bytes left on its disk; -1 when unknown, or not on a disk here
    qint64 total = -1;                   // the disk's size
    Outcome snapshot;                    // its last nightly snapshot, or a destination's last copy
    Outcome check;                       // its last light check
    Outcome dataCheck;                   // its last check that read the data back
    QDateTime checked;                   // when a light check last went through
    QDateTime dataChecked;               // when a data check last went through
    std::map<QString, QDateTime> newest; // each library's newest snapshot there, by its tag

    // How it is named to the person: its location, or a destination's name.
    [[nodiscard]] QString shown() const { return name.isEmpty() ? location : name; }
};

// A library that should have snapshots, and since when that was known.
struct Library {
    QString tag;  // Snapshots::libraryTag
    QString name; // its folder's name
    QDateTime seen;
};

struct Thresholds {
    std::chrono::hours snapshotAge{48}; // the newest snapshot older than this (the issue's 48 hours)
    std::chrono::days checkAge{15};     // no light check gone through for this long (one is due weekly)
    std::chrono::days dataCheckAge{62}; // nor one that read the data (due monthly)
    double fullBelow = 0.05;            // nearly full below this share of the disk free...
    qint64 fullFloor = qint64{1} << 30; // ...or below this many bytes, whichever is more
    std::chrono::days remindAfter{7};   // told again while it stands, this long after it was told
};

enum class Kind : std::uint8_t {
    PasswordMissing,
    Unreadable,
    DiskAbsent,
    SnapshotFailed,
    SnapshotOld,
    NoSnapshot,
    CheckFailed,
    CheckOverdue,
    DataCheckFailed,
    DataCheckOverdue,
    DiskNearlyFull,
    NoSecondDisk, // three copies on two kinds of medium, one away from home, no longer hold
    NoCopyAway,
};

struct Warning {
    Kind kind;
    QString repository; // Repository::name
    QString library;    // Library::tag, for the warnings of one library
    Failure why;        // the sentence, saying what to do

    // What stays the same while the warning stands: what it is told once by.
    [[nodiscard]] QString key() const;
};

[[nodiscard]] QString kindName(Kind kind);
[[nodiscard]] QString mediumName(Medium medium);

// The warnings at `now`, the repositories' in their order.
[[nodiscard]] QList<Warning> of(const QList<Repository> &repositories, const QList<Library> &libraries, const QDateTime &now,
                                const Thresholds &thresholds = {});

[[nodiscard]] QJsonObject json(const Warning &warning);

} // namespace SnapshotWarnings
