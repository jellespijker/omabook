// Which snapshots are kept, and when the nightly one is due (#181, ADR 0014):
// every one of the last 14 days, moment snapshots too; then the newest of 7
// days, 4 weeks and 12 months, and of every year forever.

#include <QSet>
#include <QTimeZone>
#include <QtTest>

#include "snapshotretention.h"

namespace {

using SnapshotRetention::Dated;

[[nodiscard]] QDateTime at(int year, int month, int day, int hour = 3, int minute = 0) {
    return {QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC};
}

[[nodiscard]] QString idAt(const QDateTime &time) { return time.toString(Qt::ISODate); }

// A nightly snapshot at 03:00 on every day from `first` to `last`.
[[nodiscard]] QList<Dated> nightly(const QDate &first, const QDate &last) {
    QList<Dated> snapshots;
    for (QDate day = first; day <= last; day = day.addDays(1)) {
        const QDateTime time(day, QTime(3, 0), QTimeZone::UTC);
        snapshots << Dated{.id = idAt(time), .time = time};
    }
    return snapshots;
}

[[nodiscard]] QSet<QString> kept(const QList<Dated> &snapshots, const QDateTime &now) {
    QSet<QString> ids;
    for (const Dated &snapshot : snapshots)
        ids.insert(snapshot.id);
    for (const QString &forgotten : SnapshotRetention::toForget(snapshots, now))
        ids.remove(forgotten);
    return ids;
}

} // namespace

class SnapshotRetentionTests : public QObject {
    Q_OBJECT

private slots:
    void theLastFourteenDaysAreKeptWhole() {
        // A moment snapshot every six hours for a month, besides the nightly ones.
        QList<Dated> snapshots = nightly(QDate(2026, 10, 1), QDate(2026, 10, 31));
        for (QDateTime time = at(2026, 10, 1, 1); time < at(2026, 11, 1); time = time.addSecs(6 * 3600))
            snapshots << Dated{.id = "moment " + idAt(time), .time = time};
        const QDateTime now = at(2026, 10, 31, 20);
        const QSet<QString> left = kept(snapshots, now);
        for (const Dated &snapshot : snapshots) {
            if (snapshot.time >= now.addDays(-14))
                QVERIFY2(left.contains(snapshot.id), qPrintable(snapshot.id));
        }
        // Older, only the newest of a week or of the month is left: Sunday's last.
        QVERIFY(!left.contains(idAt(at(2026, 10, 14))));
        QVERIFY(!left.contains("moment " + idAt(at(2026, 10, 10, 7))));
        QVERIFY(left.contains("moment " + idAt(at(2026, 10, 11, 19))));
        QVERIFY(!left.contains(idAt(at(2026, 10, 11))));
    }

    void yearsThinToMonthsAndEveryYearKeepsOneForever() {
        const QList<Dated> snapshots = nightly(QDate(2023, 1, 1), QDate(2025, 12, 31));
        const QSet<QString> left = kept(snapshots, at(2025, 12, 31, 12));
        // 14 days (the 18th to the 31st), the week of the 8th to the 14th
        // (the newest four weeks: 2026's first, and 2025's 52nd, 51st and
        // 50th), the newest of 11 months more, and of 2024 and 2023.
        QCOMPARE(left.size(), 14 + 1 + 11 + 2);
        for (const QDateTime &time : {at(2025, 12, 18), at(2025, 12, 14), at(2025, 11, 30), at(2025, 6, 30), at(2025, 2, 28),
                                      at(2025, 1, 31), at(2024, 12, 31), at(2023, 12, 31)})
            QVERIFY2(left.contains(idAt(time)), qPrintable(idAt(time)));
        for (const QDateTime &time : {at(2025, 12, 17), at(2025, 12, 13), at(2025, 6, 29), at(2024, 11, 30), at(2023, 12, 30)})
            QVERIFY2(!left.contains(idAt(time)), qPrintable(idAt(time)));
        // Oldest first, and only those.
        const QStringList forgotten = SnapshotRetention::toForget(snapshots, at(2025, 12, 31, 12));
        QCOMPARE(forgotten.size(), snapshots.size() - left.size());
        QCOMPARE(forgotten.first(), idAt(at(2023, 1, 1)));
    }

    void nothingIsForgottenOfAFewDays() {
        const QList<Dated> snapshots = nightly(QDate(2026, 11, 1), QDate(2026, 11, 10));
        QVERIFY(SnapshotRetention::toForget(snapshots, at(2026, 11, 10, 12)).isEmpty());
    }

    void theNightlyOneIsDueOnceANight() {
        using SnapshotRetention::nightlyDue;
        QVERIFY(nightlyDue(std::nullopt, at(2026, 11, 22, 10)));
        QVERIFY(!nightlyDue(at(2026, 11, 22, 3, 10), at(2026, 11, 22, 10)));
        QVERIFY(!nightlyDue(at(2026, 11, 21, 3, 0), at(2026, 11, 22, 2, 59)));
        QVERIFY(nightlyDue(at(2026, 11, 21, 3, 0), at(2026, 11, 22, 3, 0)));
        // A night slept through: as soon as it is asked.
        QVERIFY(nightlyDue(at(2026, 11, 19, 3, 0), at(2026, 11, 22, 17, 0)));
    }
};

QTEST_GUILESS_MAIN(SnapshotRetentionTests)
#include "snapshotretention_tests.moc"
