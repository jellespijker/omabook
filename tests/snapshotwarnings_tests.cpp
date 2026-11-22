// The warnings of a computer's snapshots (#184), worked out from what is
// known of each repository at a moment the test sets: none while all is well,
// and each of them when its repository says so, saying what to do.

#include <QtTest>
#include <algorithm>

#include "failure.h"
#include "snapshotwarnings.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using SnapshotWarnings::Kind;
using SnapshotWarnings::Medium;

namespace {

const QDateTime now(QDate(2026, 11, 23), QTime(12, 0));
const QString home = u"library:home"_s;

// A repository where all is well at `now`: snapshot an hour ago, checked a day ago.
[[nodiscard]] SnapshotWarnings::Repository fine(const QString &name = {}, Medium medium = Medium::Own) {
    SnapshotWarnings::Repository repository;
    repository.name = name;
    repository.medium = medium;
    repository.location = name.isEmpty() ? u"/srv/snapshots"_s : u"/mnt/"_s + name;
    repository.known = now.addDays(-100);
    repository.snapshot = {.ran = now.addSecs(-3600), .done = true, .said = {}};
    repository.check = {.ran = now.addDays(-1), .done = true, .said = {}};
    repository.dataCheck = repository.check;
    repository.checked = repository.check.ran;
    repository.dataChecked = repository.check.ran;
    repository.newest[home] = now.addSecs(-3600);
    repository.free = qint64{500} << 30;
    repository.total = qint64{1000} << 30;
    return repository;
}

const QList<SnapshotWarnings::Library> libraries{{.tag = home, .name = u"home"_s, .seen = now.addDays(-100)}};

[[nodiscard]] QList<Kind> kindsOf(const QList<SnapshotWarnings::Warning> &warnings) {
    QList<Kind> kinds;
    for (const auto &warning : warnings)
        kinds << warning.kind;
    return kinds;
}

[[nodiscard]] QList<Kind> kindsAt(const SnapshotWarnings::Repository &repository, const QDateTime &at = now) {
    return kindsOf(SnapshotWarnings::of({repository}, libraries, at));
}

} // namespace

class SnapshotWarningsTests : public QObject {
    Q_OBJECT

private slots:
    void noneWhileAllIsWell() {
        QVERIFY(SnapshotWarnings::of({fine()}, libraries, now).isEmpty());
        // Nothing set up, nothing to warn of.
        QVERIFY(SnapshotWarnings::of({}, libraries, now).isEmpty());
    }

    void theNewestSnapshotOlderThanTwoDays() {
        auto repository = fine();
        repository.newest[home] = now.addSecs(-47 * 3600);
        QVERIFY(kindsAt(repository).isEmpty());
        repository.newest[home] = now.addSecs(-49 * 3600);
        const auto warnings = SnapshotWarnings::of({repository}, libraries, now);
        QCOMPARE(kindsOf(warnings), QList{Kind::SnapshotOld});
        QCOMPARE(warnings.first().library, home);
        const QString text = warnings.first().why.english();
        QVERIFY2(text.contains(u"49 hours"_s) && text.contains(u"home"_s) && text.contains(u"omabook snapshot now"_s), qPrintable(text));
        // The clock goes on: two days on from a fresh one, it is old too.
        QCOMPARE(kindsAt(fine(), now.addDays(2).addSecs(3600)), QList{Kind::SnapshotOld});
    }

    void aLibraryWithoutSnapshotOnceItHadTime() {
        auto repository = fine();
        repository.newest.clear();
        const QList<SnapshotWarnings::Library> fresh{{.tag = home, .name = u"home"_s, .seen = now.addSecs(-3600)}};
        QVERIFY(SnapshotWarnings::of({repository}, fresh, now).isEmpty());
        QCOMPARE(kindsOf(SnapshotWarnings::of({repository}, libraries, now)), QList{Kind::NoSnapshot});
    }

    void aFailedNightly() {
        auto repository = fine();
        repository.snapshot = {.ran = now.addSecs(-60), .done = false, .said = u"no space left on device"_s};
        const auto warnings = SnapshotWarnings::of({repository}, libraries, now);
        QCOMPARE(kindsOf(warnings), QList{Kind::SnapshotFailed});
        QVERIFY(warnings.first().why.english().contains(u"no space left on device"_s));
    }

    void theMissingPasswordSaysWhereItGoesAndNothingElse() {
        auto repository = fine();
        repository.passwordAt = u"/home/someone/.local/state/omabook/snapshots/password"_s;
        repository.newest[home] = now.addDays(-10); // follows from it, so not said besides
        const auto warnings = SnapshotWarnings::of({repository}, libraries, now);
        QCOMPARE(kindsOf(warnings), QList{Kind::PasswordMissing});
        QVERIFY(warnings.first().why.english().contains(repository.passwordAt));
        QVERIFY(warnings.first().why.english().contains(u"recovery key"_s));
    }

    void aRepositoryThatCantBeRead() {
        auto repository = fine();
        repository.unreadable = u"repository does not exist"_s;
        QCOMPARE(kindsAt(repository), QList{Kind::Unreadable});
    }

    void aFailedOrOverdueCheck() {
        auto failed = fine();
        failed.check = {.ran = now.addSecs(-60), .done = false, .said = u"pack 1234 not found"_s};
        QCOMPARE(kindsAt(failed), QList{Kind::CheckFailed});
        auto readBadly = fine();
        readBadly.dataCheck = {.ran = now.addSecs(-60), .done = false, .said = u"pack 1234: hash does not match"_s};
        QCOMPARE(kindsAt(readBadly), QList{Kind::DataCheckFailed});

        auto late = fine();
        late.checked = now.addDays(-16);
        late.dataChecked = now.addDays(-63);
        QCOMPARE(kindsAt(late), (QList{Kind::CheckOverdue, Kind::DataCheckOverdue}));
        QVERIFY(SnapshotWarnings::of({late}, libraries, now).first().why.english().contains(u"16 days"_s));
        // Never checked: due counted from when the repository was first looked at.
        auto never = fine();
        never.checked = {};
        never.dataChecked = {};
        never.known = now.addDays(-3);
        QVERIFY(kindsAt(never).isEmpty());
        never.known = now.addDays(-20);
        QCOMPARE(kindsAt(never), QList{Kind::CheckOverdue});
    }

    void aDiskNearlyFull() {
        auto repository = fine();
        repository.free = qint64{40} << 30; // 4% of 1000 GiB
        const auto warnings = SnapshotWarnings::of({repository}, libraries, now);
        QCOMPARE(kindsOf(warnings), QList{Kind::DiskNearlyFull});
        // A small disk with room for more than a gigabyte isn't.
        repository.total = qint64{10} << 30;
        repository.free = qint64{2} << 30;
        QVERIFY(kindsAt(repository).isEmpty());
        // Not known: nothing said.
        repository.free = -1;
        QVERIFY(kindsAt(repository).isEmpty());
    }

    void aSecondDiskThatIsntThere() {
        auto disk = fine(u"usb"_s, Medium::Disk);
        disk.absent = true;
        disk.newest[home] = now.addDays(-5);
        const auto warnings = SnapshotWarnings::of({fine(), disk, fine(u"away"_s, Medium::Away)}, libraries, now);
        QCOMPARE(kindsOf(warnings), (QList{Kind::DiskAbsent, Kind::NoSecondDisk}));
        QCOMPARE(warnings.first().repository, u"usb"_s);
        QVERIFY(warnings.first().why.english().contains(u"usb"_s));
    }

    void threeCopiesTwoMediaOneAway() {
        // No destinations at all: nothing that held no longer holds.
        QVERIFY(SnapshotWarnings::of({fine()}, libraries, now).isEmpty());
        QVERIFY(SnapshotWarnings::of({fine(), fine(u"usb"_s, Medium::Disk), fine(u"away"_s, Medium::Away)}, libraries, now).isEmpty());
        QCOMPARE(kindsOf(SnapshotWarnings::of({fine(), fine(u"usb"_s, Medium::Disk)}, libraries, now)), QList{Kind::NoCopyAway});
        auto stale = fine(u"away"_s, Medium::Away);
        stale.newest[home] = now.addDays(-4);
        QCOMPARE(kindsOf(SnapshotWarnings::of({fine(), fine(u"usb"_s, Medium::Disk), stale}, libraries, now)),
                 (QList{Kind::SnapshotOld, Kind::NoCopyAway}));
    }

    void eachSaysWhatToDo() {
        auto all = fine();
        all.snapshot.done = false;
        all.snapshot.said = u"broken"_s;
        all.check.done = false;
        all.dataCheck.done = false;
        all.newest[home] = now.addDays(-5);
        all.free = 1;
        for (const auto &warning : SnapshotWarnings::of({all, fine(u"usb"_s, Medium::Disk)}, libraries, now)) {
            const QString text = warning.why.english();
            // A sentence of its own, not a code, with what to do after what is wrong.
            QVERIFY2(text.size() > 40 && text.contains(u". "_s), qPrintable(text));
            QVERIFY(!warning.key().isEmpty());
            QCOMPARE(SnapshotWarnings::json(warning).value("text"_L1).toString(), text);
        }
    }
};

QTEST_GUILESS_MAIN(SnapshotWarningsTests)
#include "snapshotwarnings_tests.moc"
