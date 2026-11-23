// How a computer's snapshots stand (#184), with restic repositories of the
// test's own and the clock set by the test: a check that finds a damaged
// repository, a nightly that failed, a password gone, a second disk that
// isn't there, the newest snapshot growing old, and each warning told once,
// then again a week on while it stands. Never the user's library or home.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <utility>

#include "bookmeta.h"
#include "fixtures.h"
#include "library.h"
#include "restic.h"
#include "snapshotdestinations.h"
#include "snapshothealth.h"
#include "snapshots.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using SnapshotWarnings::Kind;
using SnapshotWarnings::Medium;

namespace {

// A computer of the test's own: its snapshots set up, and a library with a book.
struct Computer {
    QTemporaryDir dir;
    QString state = dir.filePath(u"state"_s);
    QString repository = dir.filePath(u"repository"_s);
    SnapshotContents contents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)};
    Library library{dir.filePath(u"Library"_s)};

    [[nodiscard]] bool setUp() {
        BookMeta meta;
        meta.title = u"Kept"_s;
        Snapshots made(SnapshotPlace::in(state), contents);
        return library.open() && library.addEntry(meta, true).id > 0 && made.setUp(repository).has_value();
    }
    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::in(state), contents); }
    [[nodiscard]] QList<std::pair<QString, QString>> libraries() const { return {{Snapshots::libraryTag(library), u"Library"_s}}; }
};

[[nodiscard]] QList<Kind> kindsOf(const QList<SnapshotWarnings::Warning> &warnings) {
    QList<Kind> kinds;
    for (const auto &warning : warnings)
        kinds << warning.kind;
    return kinds;
}

// The room on the test's disk is the host's, and no part of these tests.
[[nodiscard]] SnapshotWarnings::Thresholds roomy() {
    SnapshotWarnings::Thresholds thresholds;
    thresholds.fullBelow = 0;
    thresholds.fullFloor = 0;
    return thresholds;
}

// The pack files restic keeps the data in.
[[nodiscard]] QStringList packsOf(const QString &repository) {
    QStringList packs;
    QDirIterator walk(repository + u"/data"_s, QDir::Files, QDirIterator::Subdirectories);
    while (walk.hasNext())
        packs << walk.next();
    return packs;
}

} // namespace

class SnapshotHealthTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty()); }

    void aCheckFindsADamagedRepository() {
        Computer computer;
        QVERIFY(computer.setUp());
        const Snapshots snapshots = computer.snapshots();
        QVERIFY(snapshots.take(computer.library, SnapshotReason::Asked));
        const SnapshotHealth health(snapshots);
        const QDateTime now = QDateTime::currentDateTime();
        health.check(SnapshotHealth::Depth::Light, now);
        health.check(SnapshotHealth::Depth::Data, now);
        health.look(now);
        QVERIFY(health.warnings(computer.libraries(), now, roomy()).isEmpty());
        QCOMPARE(health.repositories().first().dataChecked, now);

        // A byte of every pack turned: only reading the data back finds it.
        for (const QString &pack : packsOf(computer.repository)) {
            QFile file(pack);
            QFile::setPermissions(pack, QFile::ReadOwner | QFile::WriteOwner);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.seek(file.size() / 3));
            const char byte = file.peek(1).at(0);
            QVERIFY(file.write(QByteArray(1, static_cast<char>(~byte))) == 1);
        }
        const QDateTime later = now.addDays(1);
        health.check(SnapshotHealth::Depth::Data, later);
        auto warnings = health.warnings(computer.libraries(), later, roomy());
        QCOMPARE(kindsOf(warnings), QList{Kind::DataCheckFailed});
        QVERIFY2(warnings.first().why.english().contains(u"omabook snapshot check --read-data"_s),
                 qPrintable(warnings.first().why.english()));

        // A pack gone: the light check finds that.
        const QStringList packs = packsOf(computer.repository);
        QVERIFY(!packs.isEmpty() && QFile::remove(packs.first()));
        health.check(SnapshotHealth::Depth::Light, later);
        warnings = health.warnings(computer.libraries(), later, roomy());
        QVERIFY(kindsOf(warnings).contains(Kind::CheckFailed));
    }

    void aFailedNightlyAndTheSnapshotGrowingOld() {
        Computer computer;
        QVERIFY(computer.setUp());
        const Snapshots snapshots = computer.snapshots();
        QVERIFY(snapshots.take(computer.library, SnapshotReason::Asked));
        const SnapshotHealth health(snapshots);
        const QDateTime now = QDateTime::currentDateTime();
        health.look(now);
        health.check(SnapshotHealth::Depth::Light, now);
        health.check(SnapshotHealth::Depth::Data, now);
        QVERIFY(health.warnings(computer.libraries(), now, roomy()).isEmpty());
        // Two days and an hour on, no snapshot since: the newest is old.
        QCOMPARE(kindsOf(health.warnings(computer.libraries(), now.addSecs(49 * 3600), roomy())), QList{Kind::SnapshotOld});

        // The repository gone: the nightly fails, and says so.
        QVERIFY(QDir(computer.repository).removeRecursively());
        const auto taken = snapshots.nightly(computer.library, now.addDays(1));
        QVERIFY(!taken);
        health.noteSnapshot(std::unexpected(taken.error()), now.addDays(1));
        const auto warnings = health.warnings(computer.libraries(), now.addDays(1), roomy());
        QCOMPARE(kindsOf(warnings), QList{Kind::SnapshotFailed});
        // Looking again finds it can't be read.
        health.look(now.addDays(1));
        QVERIFY(kindsOf(health.warnings(computer.libraries(), now.addDays(1), roomy())).contains(Kind::Unreadable));
    }

    void aLibraryNeverSnapshotWarnsOnceItHadTwoDays() {
        Computer computer;
        QVERIFY(computer.setUp());
        const SnapshotHealth health(computer.snapshots());
        const QDateTime now = QDateTime::currentDateTime();
        health.look(now);
        QVERIFY(health.warnings(computer.libraries(), now, roomy()).isEmpty());
        QCOMPARE(kindsOf(health.warnings(computer.libraries(), now.addDays(3), roomy())), QList{Kind::NoSnapshot});
    }

    void thePasswordGone() {
        Computer computer;
        QVERIFY(computer.setUp());
        const QString password = computer.state + u"/password"_s;
        QVERIFY(QFile::remove(password));
        const Snapshots snapshots = computer.snapshots();
        const SnapshotHealth health(snapshots);
        QVERIFY(health.chosen());
        const QDateTime now = QDateTime::currentDateTime();
        health.check(SnapshotHealth::Depth::Light, now);
        health.look(now);
        const auto warnings = health.warnings(computer.libraries(), now, roomy());
        QCOMPARE(kindsOf(warnings), QList{Kind::PasswordMissing});
        QVERIFY(warnings.first().why.english().contains(password));
        // It wasn't checked, so no check failed.
        QVERIFY(!health.repositories().first().check.ran.isValid());
    }

    void destinationsAreMoreRepositories() {
        Computer computer;
        QVERIFY(computer.setUp());
        const Snapshots snapshots = computer.snapshots();
        QVERIFY(snapshots.take(computer.library, SnapshotReason::Asked));
        // A place away: a repository of its own, holding a snapshot of the library.
        ResticRepository away;
        away.location = computer.dir.filePath(u"away"_s);
        away.password = "away-password";
        QVERIFY(Restic::init(away));
        QVERIFY(Fixtures::write(computer.dir.filePath(u"file"_s), "kept away"));
        QVERIFY(Restic::backup(away, {.paths = {computer.dir.filePath(u"file"_s)},
                                      .excluding = {},
                                      .tags = {u"omabook"_s, Snapshots::libraryTag(computer.library)}}));
        const QDateTime copied = QDateTime::currentDateTime();
        const SnapshotHealth health(snapshots, [&] {
            return QList<SnapshotHealth::Target>{
                {.name = u"usb"_s,
                 .medium = Medium::Disk,
                 .location = u"/run/media/nobody/usb/snapshots"_s,
                 .repository = std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp)),
                 .passwordAt = {},
                 .absent = true,
                 .snapshot = SnapshotWarnings::Outcome{.ran = copied, .done = false, .said = u"not there"_s}},
                {.name = u"away"_s,
                 .medium = Medium::Away,
                 .location = away.location,
                 .repository = away,
                 .passwordAt = {},
                 .absent = false,
                 .snapshot = SnapshotWarnings::Outcome{.ran = copied, .done = true, .said = {}}},
            };
        });
        QCOMPARE(health.targets().size(), 3);
        health.look(copied);
        health.check(SnapshotHealth::Depth::Light, copied);
        health.check(SnapshotHealth::Depth::Data, copied);
        const auto repositories = health.repositories();
        QCOMPARE(repositories.size(), 3);
        QVERIFY(repositories.at(2).check.done && repositories.at(2).dataCheck.done);
        QVERIFY(repositories.at(2).newest.contains(Snapshots::libraryTag(computer.library)));
        // The disk isn't there, so three copies on two media no longer hold.
        const auto warnings = health.warnings(computer.libraries(), copied, roomy());
        QCOMPARE(kindsOf(warnings), (QList{Kind::DiskAbsent, Kind::NoSecondDisk}));
        QCOMPARE(warnings.first().repository, u"usb"_s);
        // And the status says so, each repository with what is known of it.
        const QJsonObject status = health.json(warnings);
        QCOMPARE(status.value("repositories"_L1).toArray().size(), 3);
        QCOMPARE(status.value("warnings"_L1).toArray().size(), 2);
        QCOMPARE(status.value("fine"_L1).toBool(true), false);
    }

    void theDestinationsKeptBesideThemAreLookedAfterToo() {
        Computer computer;
        QVERIFY(computer.setUp());
        const Snapshots snapshots = computer.snapshots();
        QVERIFY(snapshots.take(computer.library, SnapshotReason::Asked));
        // A second disk, added as `omabook snapshot destination add` does, with a copy on it.
        SnapshotDestinations destinations(snapshots.place());
        const QString disk = computer.dir.filePath(u"usb/omabook"_s);
        QVERIFY(QDir().mkpath(computer.dir.filePath(u"usb"_s)));
        const auto added = destinations.add({.name = u"usb"_s, .location = disk, .credentials = {}}, {});
        QVERIFY2(added, qPrintable(Fixtures::whyNot(added)));
        const QDateTime now = QDateTime::currentDateTime();
        const auto copied = destinations.copy(u"usb"_s, now);
        QVERIFY2(copied, qPrintable(Fixtures::whyNot(copied)));

        // Without being handed them, the health finds them beside the snapshots.
        const SnapshotHealth health(snapshots);
        QCOMPARE(health.targets().size(), 2);
        health.look(now);
        health.check(SnapshotHealth::Depth::Light, now);
        health.check(SnapshotHealth::Depth::Data, now);
        const auto repositories = health.repositories();
        QCOMPARE(repositories.at(1).name, u"usb"_s);
        QCOMPARE(repositories.at(1).medium, Medium::Disk);
        QVERIFY(repositories.at(1).snapshot.done && repositories.at(1).check.done && repositories.at(1).dataCheck.done);
        QVERIFY(repositories.at(1).newest.contains(Snapshots::libraryTag(computer.library)));
        // The disk holds; nothing is kept away from home.
        QCOMPARE(kindsOf(health.warnings(computer.libraries(), now, roomy())), QList{Kind::NoCopyAway});

        // Unplugged: its folder gone. The next copy finds it isn't there, and so do the warnings.
        QVERIFY(QDir().rename(computer.dir.filePath(u"usb"_s), computer.dir.filePath(u"unplugged"_s)));
        QVERIFY(!destinations.copy(u"usb"_s, now.addDays(1)));
        health.look(now.addDays(1));
        const auto warnings = health.warnings(computer.libraries(), now.addDays(1), roomy());
        QCOMPARE(kindsOf(warnings), (QList{Kind::DiskAbsent, Kind::NoSecondDisk, Kind::NoCopyAway}));
        QVERIFY(warnings.first().why.english().contains(disk));

        // Plugged in again: it holds again once copied to.
        QVERIFY(QDir().rename(computer.dir.filePath(u"unplugged"_s), computer.dir.filePath(u"usb"_s)));
        QVERIFY(destinations.copy(u"usb"_s, now.addDays(1)));
        health.look(now.addDays(1));
        QCOMPARE(kindsOf(health.warnings(computer.libraries(), now.addDays(1), roomy())), QList{Kind::NoCopyAway});

        // Its password gone: said, with where it goes.
        QVERIFY(QFile::remove(computer.state + u"/secrets/"_s + SnapshotDestinations::passwordSecret(u"usb"_s)));
        const auto locked = health.warnings(computer.libraries(), now.addDays(1), roomy());
        QVERIFY(kindsOf(locked).contains(Kind::PasswordMissing));
        QCOMPARE(locked.first().repository, u"usb"_s);
    }

    void eachWarningIsToldOnceThenAgainAWeekOn() {
        Computer computer;
        QVERIFY(computer.setUp());
        const SnapshotHealth health(computer.snapshots());
        const QDateTime now = QDateTime::currentDateTime();
        health.noteSnapshot(std::unexpected(Failure(Failure::Code::ResticFailed, {u"disk full"_s})), now);
        const auto warnings = health.warnings(computer.libraries(), now, roomy());
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(health.toTell(warnings, now).size(), 1);
        QVERIFY(health.toTell(warnings, now.addSecs(3600)).isEmpty());
        QVERIFY(health.toTell(warnings, now.addDays(6)).isEmpty());
        QCOMPARE(health.toTell(warnings, now.addDays(7)).size(), 1);
        // Gone, then back: new again.
        QVERIFY(health.toTell({}, now.addDays(8)).isEmpty());
        QCOMPARE(health.toTell(warnings, now.addDays(8)).size(), 1);
    }

    void aDataCheckReadsAllOfADiskAndAMonthsTwelfthAway() {
        QCOMPARE(SnapshotHealth::readDataOf(Medium::Own, QDateTime(QDate(2026, 3, 9), QTime(3, 0))), u"all"_s);
        QCOMPARE(SnapshotHealth::readDataOf(Medium::Disk, QDateTime(QDate(2026, 3, 9), QTime(3, 0))), u"all"_s);
        QCOMPARE(SnapshotHealth::readDataOf(Medium::Away, QDateTime(QDate(2026, 3, 9), QTime(3, 0))), u"3/12"_s);
        QCOMPARE(SnapshotHealth::readDataOf(Medium::Away, QDateTime(QDate(2026, 12, 1), QTime(3, 0))), u"12/12"_s);
    }

    void theCommandSaysHowTheSnapshotsStand() {
        QTemporaryDir dir;
        const QString library = dir.filePath(u"Library"_s);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const auto &[name, folder] : {std::pair{"HOME", "home"},
                                           {"XDG_CONFIG_HOME", "home/.config"},
                                           {"XDG_CACHE_HOME", "home/.cache"},
                                           {"XDG_DATA_HOME", "home/.local/share"},
                                           {"XDG_STATE_HOME", "home/.local/state"}})
            environment.insert(QString::fromLatin1(name), dir.filePath(QString::fromLatin1(folder)));
        environment.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        environment.remove(u"CREDENTIALS_DIRECTORY"_s);
        // No session bus of the person's: no notification leaves the test.
        environment.insert(u"DBUS_SESSION_BUS_ADDRESS"_s, u"unix:path="_s + dir.filePath(u"no-bus"_s));
        QVERIFY(QDir().mkpath(dir.filePath(u"home/.config/omabook"_s)));
        QVERIFY(Fixtures::write(dir.filePath(u"home/.config/omabook/config.json"_s), "{\"ai_provider\": \"off\"}"));
        const auto omabook = [&environment](const QStringList &arguments) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(QStringLiteral(OMABOOK_BINARY), arguments);
            process.closeWriteChannel();
            process.waitForFinished(120000);
            // Standard output alone: standard error tells the warnings besides.
            return std::pair{process.exitCode(), QString::fromUtf8(process.readAllStandardOutput())};
        };
        QVERIFY(Fixtures::write(dir.filePath(u"A Book - Some One.txt"_s), "Words.\n"));
        QCOMPARE(omabook({u"add"_s, u"--library"_s, library, dir.filePath(u"A Book - Some One.txt"_s)}).first, 0);
        // Not set up: said, and no warning.
        const auto [unset, unsetSaid] = omabook({u"snapshot"_s, u"status"_s, u"--library"_s, library});
        QVERIFY2(unset == 0 && unsetSaid.contains(u"aren't set up"_s), qPrintable(unsetSaid));
        QCOMPARE(omabook({u"snapshot"_s, u"setup"_s, u"--repository"_s, dir.filePath(u"repository"_s)}).first, 0);
        QCOMPARE(omabook({u"snapshot"_s, u"now"_s, u"--library"_s, library}).first, 0);
        // Checked, reading the data back: nothing to see to, but for the room on the host's disk, which is no part of the test.
        const auto [checked, checkSaid] = omabook({u"snapshot"_s, u"check"_s, u"--read-data"_s, u"--json"_s, u"--library"_s, library});
        const QJsonObject check = QJsonDocument::fromJson(checkSaid.toUtf8()).object();
        QVERIFY2(check.value("repositories"_L1).toArray().first()["data_check"_L1]["done"_L1].toBool(), qPrintable(checkSaid));
        for (const auto &warning : check.value("warnings"_L1).toArray())
            QVERIFY2(warning["kind"_L1].toString() == "disk-nearly-full"_L1, qPrintable(checkSaid));
        QVERIFY2(checked == 0 || !check.value("warnings"_L1).toArray().isEmpty(), qPrintable(checkSaid));
        const auto [fine, fineSaid] = omabook({u"snapshot"_s, u"status"_s, u"--json"_s, u"--library"_s, library});
        const QJsonObject status = QJsonDocument::fromJson(fineSaid.toUtf8()).object();
        // Fine, but for the room on the host's disk, which is no part of the test.
        for (const auto &warning : status.value("warnings"_L1).toArray())
            QVERIFY2(warning["kind"_L1].toString() == "disk-nearly-full"_L1, qPrintable(fineSaid));
        QVERIFY2(fine == 0 || !status.value("warnings"_L1).toArray().isEmpty(), qPrintable(fineSaid));
        QVERIFY(status.value("set_up"_L1).toBool());
        QVERIFY(!status.value("repositories"_L1).toArray().first()["newest"_L1].toString().isEmpty());

        // The password gone: said in the status, with where it goes, and the command fails.
        QVERIFY(QFile::remove(dir.filePath(u"home/.local/state/omabook/snapshots/password"_s)));
        const auto [missing, missingSaid] = omabook({u"snapshot"_s, u"status"_s, u"--library"_s, library});
        QVERIFY2(missing != 0 && missingSaid.contains(u"recovery key"_s), qPrintable(missingSaid));
    }
};

QTEST_GUILESS_MAIN(SnapshotHealthTests)
#include "snapshothealth_tests.moc"
