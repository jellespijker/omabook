// A sync takes a moment snapshot of this computer's library before it writes
// to it, however it is started (#291): the command line's `sync`, a sync with
// a hub (hubsync_tests), and what the window, automatic sync and an agent
// call. A dry run takes none. In folders and repositories of the test's own,
// never the user's library or home.

#include <QDir>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "bookmeta.h"
#include "fixtures.h"
#include "library.h"
#include "momentsnapshot.h"
#include "remote.h"
#include "snapshots.h"
#include "sync.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

// This computer and another, "laptop", behind a stand-in ssh, with this one's
// home, and so its snapshots, in a folder of the test's own.
struct Computers {
    QTemporaryDir dir;
    Library here{dir.filePath(u"Here"_s)};
    Library there{dir.filePath(u"There"_s)};
    QString ssh = Fixtures::standInSsh(dir.path(), QString::fromLatin1(OMABOOK_BINARY));

    [[nodiscard]] bool made() {
        for (const auto &[name, folder] : {std::pair{"HOME", "home"},
                                           {"XDG_CONFIG_HOME", "home/.config"},
                                           {"XDG_DATA_HOME", "home/.local/share"},
                                           {"XDG_CACHE_HOME", "home/.cache"},
                                           {"XDG_STATE_HOME", "home/.local/state"}}) {
            if (!QDir().mkpath(dir.filePath(QString::fromLatin1(folder))))
                return false;
            qputenv(name, dir.filePath(QString::fromLatin1(folder)).toUtf8());
        }
        qunsetenv("CREDENTIALS_DIRECTORY");
        BookMeta meta;
        meta.title = u"The Salt Road"_s;
        return here.open() && there.open() && there.addEntry(meta, true).id > 0;
    }

    [[nodiscard]] Remote remote() const { return Remote(u"laptop:"_s + dir.filePath(u"There"_s), {ssh}); }

    // This computer's omabook, its ssh the stand-in.
    [[nodiscard]] std::pair<int, QString> omabook(const QStringList &arguments) const {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"PATH"_s, dir.path() + u':' + environment.value(u"PATH"_s));
        environment.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(QString::fromLatin1(OMABOOK_BINARY), arguments);
        process.closeWriteChannel();
        process.waitForFinished(120000);
        return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
    }

    // The reasons of this library's snapshots, oldest first.
    [[nodiscard]] QStringList reasons() const {
        QStringList found;
        if (const auto listed = Snapshots(SnapshotPlace::here()).list(here))
            for (const Restic::Snapshot &snapshot : *listed)
                found << snapshot.tagged(u"reason:"_s);
        return found;
    }

    [[nodiscard]] bool setUpSnapshots() const {
        Snapshots snapshots(SnapshotPlace::here());
        return snapshots.setUp(dir.filePath(u"repository"_s)).has_value();
    }
};

[[nodiscard]] QString said(const SyncSummary &summary) { return (QStringList{summary.error} + summary.problems).join(u'\n'); }

} // namespace

class SyncSnapshotTests : public QObject {
    Q_OBJECT

private slots:
    void aSyncTakesAMomentSnapshotBeforeItWrites() {
        Computers computers;
        QVERIFY(computers.made() && computers.setUpSnapshots());
        SyncOptions dry;
        dry.dryRun = true;
        const SyncSummary seen = syncLibrary(computers.here, computers.remote(), {}, dry);
        QVERIFY2(said(seen).isEmpty(), qPrintable(said(seen)));
        QVERIFY(seen.momentSnapshot.isEmpty() && computers.reasons().isEmpty());

        const SyncSummary synced = syncLibrary(computers.here, computers.remote());
        QVERIFY2(said(synced).isEmpty(), qPrintable(said(synced)));
        QCOMPARE(synced.received, 1);
        QVERIFY(!synced.momentSnapshot.isEmpty() && synced.noSnapshot.isEmpty());
        QCOMPARE(computers.reasons(), QStringList{u"sync"_s});
    }

    // Without snapshots set up, a sync goes on without one, and says nothing of it.
    void aSyncWithoutSnapshotsSetUpSaysNothingOfThem() {
        Computers computers;
        QVERIFY(computers.made());
        const SyncSummary synced = syncLibrary(computers.here, computers.remote());
        QVERIFY2(said(synced).isEmpty(), qPrintable(said(synced)));
        QCOMPARE(synced.received, 1);
        QVERIFY(synced.momentSnapshot.isEmpty() && synced.noSnapshot.isEmpty());
    }

    // Set up, and the password gone: said so, never "not set up", and a sync
    // that goes on without a snapshot says why (#296).
    void aSnapshotPasswordGoneIsSaid() {
        Computers computers;
        QVERIFY(computers.made() && computers.setUpSnapshots());
        QVERIFY(QFile::remove(SnapshotPlace::here().repository.passwordFile));
        const Snapshots snapshots(SnapshotPlace::here());
        const auto taken = snapshots.take(computers.here, SnapshotReason::Asked);
        QVERIFY(!taken && taken.error().code() == Failure::Code::SnapshotPasswordMissing);
        const auto listed = snapshots.list(computers.here);
        QVERIFY(!listed && listed.error().code() == Failure::Code::SnapshotPasswordMissing);
        const auto nightly = snapshots.nightly(computers.here, QDateTime::currentDateTime());
        QVERIFY(!nightly);
        // Before an upgrade it is refused, as any snapshot that can't be taken; before a bulk change, warned of.
        const auto upgrade = MomentSnapshot::beforeUpgrade(snapshots, computers.here);
        QVERIFY(!upgrade && upgrade.error().code() == Failure::Code::UpgradeNotSnapshot);
        const auto bulk = MomentSnapshot::beforeBulk(snapshots, computers.here);
        QVERIFY(bulk && bulk->unsnapshot);

        const SyncSummary synced = syncLibrary(computers.here, computers.remote());
        QVERIFY2(said(synced).isEmpty(), qPrintable(said(synced)));
        QCOMPARE(synced.received, 1);
        QVERIFY2(synced.noSnapshot.contains(u"password isn't in"_s), qPrintable(synced.noSnapshot));
        for (const QStringList &command : {QStringList{u"snapshot"_s, u"now"_s}, {u"snapshot"_s, u"list"_s}, {u"snapshot"_s, u"setup"_s}}) {
            const auto [code, output] = computers.omabook(command + QStringList{u"--library"_s, computers.here.root()});
            QVERIFY2(code != 0 && output.contains(u"password isn't in"_s), qPrintable(output));
        }
        const auto [code, output] =
            computers.omabook({u"sync"_s, u"--library"_s, computers.here.root(), u"laptop:"_s + computers.there.root()});
        QVERIFY2(code == 0 && output.count(u"No moment snapshot could be taken first"_s) == 1, qPrintable(output));
    }

    // One snapshot, said once, by `omabook sync`.
    void theCommandLineSaysItsSnapshotOnce() {
        Computers computers;
        QVERIFY(computers.made() && computers.setUpSnapshots());
        const auto [code, output] =
            computers.omabook({u"sync"_s, u"--library"_s, computers.here.root(), u"laptop:"_s + computers.there.root()});
        QVERIFY2(code == 0, qPrintable(output));
        QVERIFY2(output.count(u"A moment snapshot of the library was taken first"_s) == 1, qPrintable(output));
        QCOMPARE(computers.reasons(), QStringList{u"sync"_s});
    }
};

QTEST_GUILESS_MAIN(SyncSnapshotTests)
#include "syncsnapshot_tests.moc"
