// The hub's snapshots (#181) through its daemon's seams (#179): set up with
// the password one of the hub's secrets, a moment snapshot taken in the
// keeper's lock before a write that changes much (and holding the library
// as it was before it), and the nightly job copying every library at once
// in the lock and keeping them with restic after it. In folders of the
// test's own, never the user's library or home.

#include <QDir>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "bookmeta.h"
#include "fixtures.h"
#include "hubkeeper.h"
#include "hubplace.h"
#include "hubsnapshots.h"
#include "library.h"
#include "libraryhold.h"
#include "snapshots.h"

using namespace Qt::StringLiterals;

namespace {

struct Hub {
    QTemporaryDir dir;
    QString libraries = dir.filePath(u"libraries"_s);
    HubPlace place{{.state = dir.filePath(u"state/omabook-hub"_s), .runtime = dir.filePath(u"runtime"_s), .credentials = {}}};
    SnapshotContents contents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)};

    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::ofHub(place), contents); }

    // A library called `name` in the hub's folder, with a book.
    [[nodiscard]] bool library(const QString &name) const {
        Library library(QDir(libraries).filePath(name));
        BookMeta meta;
        meta.title = u"Kept in "_s + name;
        return library.open() && library.addEntry(meta, true).id > 0;
    }
};

[[nodiscard]] QJsonObject addOne(Library &library) {
    BookMeta meta;
    meta.title = u"Synced in"_s;
    return QJsonObject{{u"id"_s, library.addEntry(meta, true).id}};
}

} // namespace

class HubSnapshotsTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty()); }

    void theHubsPasswordIsOneOfItsSecrets() {
        const Hub hub;
        Snapshots snapshots = hub.snapshots();
        QVERIFY(!snapshots.ready());
        const auto key = snapshots.setUp(hub.dir.filePath(u"repository"_s));
        QVERIFY2(key, qPrintable(Fixtures::whyNot(key)));
        const auto kept = hub.place.secret(u"snapshots"_s);
        QVERIFY2(kept, qPrintable(Fixtures::whyNot(kept)));
        QCOMPARE(QString::fromLatin1(*kept), *key);
        // Read again as the daemon starts: set up, with restic handed the secret.
        QVERIFY(hub.snapshots().ready());
        QVERIFY(!QFileInfo::exists(hub.place.state() + u"/snapshots/password"_s));
    }

    void aWriteThatChangesMuchIsSnapshotFirstInTheLock() {
        const Hub hub;
        QVERIFY(hub.library(u"home"_s));
        Snapshots made = hub.snapshots();
        QVERIFY(made.setUp(hub.dir.filePath(u"repository"_s)));
        const Snapshots snapshots = hub.snapshots();
        HubKeeper keeper(hub.libraries);
        keeper.before(HubSnapshots::before(snapshots));
        QVERIFY(keeper.open());
        QVERIFY(keeper.write(u"home"_s, HubKeeper::Moment::None, addOne));
        QVERIFY(keeper.write(u"home"_s, HubKeeper::Moment::Sync, addOne));

        Library *home = nullptr;
        QVERIFY(keeper.write(u"home"_s, HubKeeper::Moment::None, [&home](Library &library) -> std::expected<QJsonObject, Failure> {
            home = &library;
            return QJsonObject{};
        }));
        const auto listed = snapshots.list(*home);
        QVERIFY2(listed, qPrintable(Fixtures::whyNot(listed)));
        QCOMPARE(listed->size(), 1);
        QCOMPARE(listed->first().tagged(u"reason:"_s), u"sync"_s);
        // The hub's hold on the library is no part of it.
        const QString target = hub.dir.filePath(u"restored"_s);
        QProcess restic;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"RESTIC_PASSWORD"_s, QString::fromLatin1(*hub.place.secret(u"snapshots"_s)));
        restic.setProcessEnvironment(environment);
        restic.start(u"restic"_s,
                     {u"--repo"_s, hub.dir.filePath(u"repository"_s), u"restore"_s, listed->first().id, u"--target"_s, target});
        QVERIFY(restic.waitForFinished(60000) && restic.exitCode() == 0);
        QVERIFY(QFileInfo::exists(target + home->root()));
        QVERIFY(!QFileInfo::exists(target + home->root() + u'/' + QString::fromLatin1(LibraryHold::fileName)));
        // Two books before the sync wrote its third.
        const QString database =
            target + hub.place.state() + u"/snapshots/staging/"_s + Snapshots::libraryTag(*home).section(u':', 1) + u"/omabook.db"_s;
        const QString copy = hub.dir.filePath(u"copy"_s);
        QVERIFY(QDir().mkpath(copy) && QFile::copy(database, Library::databaseIn(copy)));
        Library restored(copy);
        restored.keepFormat();
        QVERIFY(restored.open());
        QCOMPARE(restored.books().size(), 2);
    }

    void aWriteIsRefusedWhenItsSnapshotCantBeTaken() {
        const Hub hub;
        QVERIFY(hub.library(u"home"_s));
        Snapshots made = hub.snapshots();
        QVERIFY(made.setUp(hub.dir.filePath(u"repository"_s)));
        // The repository gone: no snapshot, so no write.
        QVERIFY(QDir(hub.dir.filePath(u"repository"_s)).removeRecursively());
        const Snapshots snapshots = hub.snapshots();
        HubKeeper keeper(hub.libraries);
        keeper.before(HubSnapshots::before(snapshots));
        QVERIFY(keeper.open());
        const auto refused = keeper.write(u"home"_s, HubKeeper::Moment::Import, addOne);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code(), Failure::Code::ResticFailed);
    }

    void theNightlyJobKeepsEveryLibrary() {
        const Hub hub;
        QVERIFY(hub.library(u"home"_s) && hub.library(u"work"_s));
        HubKeeper keeper(hub.libraries);
        QVERIFY(keeper.open());
        const Snapshots unset = hub.snapshots();
        // Not set up: nothing to do, and nothing failed.
        QVERIFY(HubSnapshots::nightly(unset)(keeper));
        Snapshots made = hub.snapshots();
        QVERIFY(made.setUp(hub.dir.filePath(u"repository"_s)));
        const Snapshots snapshots = hub.snapshots();
        const auto ran = HubSnapshots::nightly(snapshots)(keeper);
        QVERIFY2(ran, qPrintable(Fixtures::whyNot(ran)));
        QVERIFY(!keeper.busy());
        for (const QString &name : {u"home"_s, u"work"_s}) {
            QVERIFY(
                keeper.write(name, HubKeeper::Moment::None, [&snapshots, &name](Library &library) -> std::expected<QJsonObject, Failure> {
                    const auto listed = snapshots.list(library);
                    if (!listed || listed->size() != 1 || !listed->first().tags.contains(u"nightly"_s))
                        return std::unexpected(Failure::said(name + u": no nightly snapshot"_s));
                    return QJsonObject{};
                }));
        }
    }
};

QTEST_GUILESS_MAIN(HubSnapshotsTests)
#include "hubsnapshots_tests.moc"
