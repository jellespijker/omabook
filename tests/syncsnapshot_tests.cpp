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

    // One snapshot, said once, by `omabook sync`.
    void theCommandLineSaysItsSnapshotOnce() {
        Computers computers;
        QVERIFY(computers.made() && computers.setUpSnapshots());
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"PATH"_s, computers.dir.path() + u':' + environment.value(u"PATH"_s));
        environment.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(QString::fromLatin1(OMABOOK_BINARY),
                      {u"sync"_s, u"--library"_s, computers.here.root(), u"laptop:"_s + computers.there.root()});
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(120000));
        const QString output = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
        QVERIFY2(process.exitCode() == 0, qPrintable(output));
        QVERIFY2(output.count(u"A moment snapshot of the library was taken first"_s) == 1, qPrintable(output));
        QCOMPARE(computers.reasons(), QStringList{u"sync"_s});
    }
};

QTEST_GUILESS_MAIN(SyncSnapshotTests)
#include "syncsnapshot_tests.moc"
