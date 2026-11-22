#include "snapshotretention.h"

#include <QDate>
#include <QSet>
#include <QTime>
#include <QTimeZone>
#include <algorithm>
#include <functional>
#include <ranges>

namespace {

// Keeps the newest of each of the first `count` periods (as `period` names
// them) among `newestFirst`; every period when `count` is negative.
void keepNewestOf(const QList<SnapshotRetention::Dated> &newestFirst, int count, const std::function<qint64(const QDate &)> &period,
                  QSet<QString> &kept) {
    QSet<qint64> seen;
    for (const SnapshotRetention::Dated &snapshot : newestFirst) {
        if (count >= 0 && seen.size() >= count)
            return;
        if (const qint64 its = period(snapshot.time.date()); !seen.contains(its)) {
            seen.insert(its);
            kept.insert(snapshot.id);
        }
    }
}

} // namespace

QStringList SnapshotRetention::toForget(QList<Dated> snapshots, const QDateTime &now) {
    std::ranges::stable_sort(snapshots, std::greater{}, &Dated::time);
    QSet<QString> kept;
    const QDateTime recent = now.addDuration(-std::chrono::duration_cast<std::chrono::milliseconds>(allKept));
    for (const Dated &snapshot : snapshots)
        if (snapshot.time >= recent)
            kept.insert(snapshot.id);
    keepNewestOf(snapshots, days, &QDate::toJulianDay, kept);
    keepNewestOf(
        snapshots, weeks,
        [](const QDate &date) {
            int year = 0;
            const int week = date.weekNumber(&year);
            return (qint64{year} * 100) + week;
        },
        kept);
    keepNewestOf(snapshots, months, [](const QDate &date) { return (qint64{date.year()} * 100) + date.month(); }, kept);
    keepNewestOf(snapshots, -1, [](const QDate &date) { return qint64{date.year()}; }, kept);
    QStringList forgotten;
    for (const Dated &snapshot : std::views::reverse(snapshots))
        if (!kept.contains(snapshot.id))
            forgotten << snapshot.id;
    return forgotten;
}

bool SnapshotRetention::nightlyDue(const std::optional<QDateTime> &last, const QDateTime &now) {
    QDateTime night(now.date(), QTime(nightHour, 0), now.timeZone());
    if (now < night)
        night = night.addDays(-1);
    return !last || *last < night;
}
