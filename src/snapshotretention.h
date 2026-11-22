#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <chrono>
#include <optional>

// Which snapshots are kept, and when the nightly one is due (ADR 0014): rules
// about time alone, so they are the same on a hub and on a laptop, and tested
// without restic.
namespace SnapshotRetention {

// Every snapshot of the last 14 days is kept: the moment snapshots are kept
// that long. Of all of them, then, the newest of each of the 7 newest days
// that have one, of the 4 newest weeks, of the 12 newest months, and of every
// year, forever. Days, weeks and months are those of the snapshot's own time.
inline constexpr auto allKept = std::chrono::days(14);
inline constexpr int days = 7, weeks = 4, months = 12;

struct Dated {
    QString id;
    QDateTime time;
};

// The ids of the snapshots that are kept no longer at `now`, oldest first.
[[nodiscard]] QStringList toForget(QList<Dated> snapshots, const QDateTime &now);

// The hour of the night the nightly snapshot is taken from, in `now`'s time zone.
inline constexpr int nightHour = 3;

// Whether a nightly snapshot is due at `now`, the last one taken at `last`:
// none since the latest night hour, and so the one after a night the computer
// slept through as soon as it is asked.
[[nodiscard]] bool nightlyDue(const std::optional<QDateTime> &last, const QDateTime &now);

} // namespace SnapshotRetention
