// The moment snapshots before an upgrade and before an agent's change to
// many books (#208, ADR 0014): a library in an older format is snapshot
// before it is brought up to date, and left as it was when that can't be
// done (on a hub and on a computer with snapshots set up alike), or brought
// up to date with its copy in .backups where snapshots aren't set up; a
// change to more than MomentSnapshot::bulkBooks books by an MCP tool or a
// command takes one first and says it as the undo, refused on a hub when it
// can't, warned of on a computer. In folders and repositories of the test's
// own, never the user's library or home.

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <utility>

#include "bookmeta.h"
#include "fixtures.h"
#include "hubkeeper.h"
#include "hubplace.h"
#include "hubsnapshots.h"
#include "library.h"
#include "librarymigrations.h"
#include "mcpharness.h"
#include "momentsnapshot.h"
#include "restic.h"
#include "samples.h"
#include "snapshotcommands.h"
#include "snapshots.h"

using namespace Qt::StringLiterals;
using McpHarness::Machine;
using McpHarness::Server;

namespace {

// The 0.1.5 sample, in format 12, as the library in `root`.
[[nodiscard]] bool olderLibrary(const QString &root) {
    int format = 0;
    if (!QDir().mkpath(root))
        return false;
    Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), Library::databaseIn(root), format);
    return format == 12;
}

// The format of the database in `file`, read without opening it as a library.
[[nodiscard]] int formatOf(const QString &file) {
    int format = -1;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"format"_s);
        database.setDatabaseName(file);
        database.setConnectOptions(u"QSQLITE_OPEN_READONLY"_s);
        if (database.open()) {
            QSqlQuery version(database);
            if (version.exec(u"PRAGMA user_version"_s) && version.next())
                format = version.value(0).toInt();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(u"format"_s);
    return format;
}

// A computer of the test's own: its snapshots' state and repository.
struct Computer {
    QTemporaryDir dir;
    QString library = dir.filePath(u"Library"_s);
    QString state = dir.filePath(u"state/omabook/snapshots"_s);
    QString repository = dir.filePath(u"repository"_s);
    SnapshotContents contents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)};

    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::in(state), contents); }
    [[nodiscard]] bool setUp() const {
        Snapshots made = snapshots();
        return made.setUp(repository).has_value();
    }

    // The database the snapshot `id` holds, restored into a folder of its own.
    [[nodiscard]] QString restoredDatabase(const QString &id) const {
        const QString target = dir.filePath(u"restored-"_s + id.left(8));
        QProcess restic;
        restic.start(u"restic"_s,
                     {u"--repo"_s, repository, u"--password-file"_s, state + u"/password"_s, u"restore"_s, id, u"--target"_s, target});
        if (!restic.waitForFinished(60000) || restic.exitCode() != 0)
            return {};
        for (QDirIterator found(target, {u"omabook.db"_s}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); found.hasNext();)
            if (const QString file = found.next(); file.contains(u"/staging/"_s))
                return file;
        return {};
    }
};

// This computer's moment snapshot before every upgrade, as main() sets it,
// with the test's snapshots, while it lives.
class Watching {
public:
    explicit Watching(const Snapshots &snapshots)
        : m_previous(LibraryMigrations::beforeUpgrading(
              [&snapshots](Library &library) { return MomentSnapshot::beforeUpgrade(snapshots, library); })) {}
    ~Watching() { LibraryMigrations::beforeUpgrading(m_previous); }
    Watching(const Watching &) = delete;
    Watching &operator=(const Watching &) = delete;
    Watching(Watching &&) = delete;
    Watching &operator=(Watching &&) = delete;

private:
    LibraryMigrations::BeforeUpgrade m_previous;
};

struct Hub {
    QTemporaryDir dir;
    QString libraries = dir.filePath(u"libraries"_s);
    HubPlace place{{.state = dir.filePath(u"state/omabook-hub"_s), .runtime = dir.filePath(u"runtime"_s), .credentials = {}}};
    SnapshotContents contents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)};

    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::ofHub(place), contents); }
    [[nodiscard]] bool setUp() const {
        Snapshots made = snapshots();
        return made.setUp(dir.filePath(u"repository"_s)).has_value();
    }
};

[[nodiscard]] qint64 entry(Library &library, const QString &title) {
    BookMeta meta;
    meta.title = title;
    return library.addEntry(meta, true).id;
}

// The reasons of the library's snapshots, oldest first.
[[nodiscard]] QStringList reasons(const Snapshots &snapshots, const Library &library) {
    QStringList found;
    if (const auto listed = snapshots.list(library))
        for (const Restic::Snapshot &snapshot : *listed)
            found << snapshot.tagged(u"reason:"_s);
    return found;
}

} // namespace

class MomentSnapshotTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty()); }

    void aBulkChangeIsMoreThanTheThreshold() {
        QVERIFY(!MomentSnapshot::isBulk(MomentSnapshot::bulkBooks));
        QVERIFY(MomentSnapshot::isBulk(MomentSnapshot::bulkBooks + 1));
        // As a command an agent runs says it.
        const auto of = [](const QStringList &words) { return riskyMoment(QStringList{u"omabook"_s} + words); };
        QStringList many, few;
        for (int id = 1; id <= MomentSnapshot::bulkBooks + 1; ++id)
            (id <= MomentSnapshot::bulkBooks ? few : many) << QString::number(id);
        many = few + many;
        QVERIFY(of(QStringList{u"collections"_s, u"include"_s, u"Shelf"_s} + many) == SnapshotReason::BulkChange);
        QVERIFY(of(QStringList{u"collections"_s, u"--library"_s, u"/books"_s, u"exclude"_s, u"Shelf"_s} + many)
                == SnapshotReason::BulkChange);
        QVERIFY(of(QStringList{u"collections"_s, u"follow"_s, u"Shelf"_s} + many) == SnapshotReason::BulkChange);
        QVERIFY(!of(QStringList{u"collections"_s, u"include"_s, u"Shelf"_s} + few));
        QVERIFY(of(QStringList{u"embed"_s} + many) == SnapshotReason::BulkChange);
        QVERIFY(of({u"embed"_s, u"--all"_s}) == SnapshotReason::BulkChange);
        QVERIFY(!of({u"embed"_s, u"3"_s}));
        QVERIFY(of({u"collections"_s, u"sort"_s, u"--apply"_s}) == SnapshotReason::BulkChange);
        QVERIFY(of({u"collections"_s, u"sort"_s, u"--pick"_s, u"1,2"_s}) == SnapshotReason::BulkChange);
        QVERIFY(!of({u"collections"_s, u"sort"_s}));
        QVERIFY(!of({u"collections"_s, u"list"_s}));
    }

    void anUpgradeIsSnapshotFirst() {
        const Computer computer;
        QVERIFY(computer.setUp() && olderLibrary(computer.library));
        const Snapshots snapshots = computer.snapshots();
        const Watching watching(snapshots);
        Library library(computer.library);
        QStringList said;
        library.onBringingUpToDate([&said](const QString &line) { said << line; });
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
        QVERIFY2(!said.isEmpty() && said.first().contains(u"moment snapshot"_s), qPrintable(said.join(u'\n')));
        const auto listed = snapshots.list(library);
        QVERIFY2(listed, qPrintable(Fixtures::whyNot(listed)));
        QCOMPARE(listed->size(), 1);
        QCOMPARE(listed->first().tagged(u"reason:"_s), u"upgrade"_s);
        // It holds the library as it was: in its older format.
        const QString database = computer.restoredDatabase(listed->first().id);
        QVERIFY(!database.isEmpty());
        QCOMPARE(formatOf(database), 12);
    }

    void anUpgradeIsRefusedWhenItsSnapshotCantBeTaken() {
        const Computer computer;
        QVERIFY(computer.setUp() && olderLibrary(computer.library));
        // The repository gone: no snapshot, so no upgrade.
        QVERIFY(QDir(computer.repository).removeRecursively());
        const Snapshots snapshots = computer.snapshots();
        const Watching watching(snapshots);
        {
            Library library(computer.library);
            QString error;
            QVERIFY(!library.open(&error));
            QVERIFY2(error.contains(u"left alone"_s) && error.contains(u"moment snapshot"_s), qPrintable(error));
        }
        QCOMPARE(formatOf(Library::databaseIn(computer.library)), 12);
        QVERIFY(!QFileInfo::exists(computer.library + u"/.backups"_s));
    }

    void withoutSnapshotsAnUpgradeGoesOnWithItsCopy() {
        const Computer computer;
        QVERIFY(olderLibrary(computer.library));
        const Snapshots snapshots = computer.snapshots();
        QVERIFY(!snapshots.ready());
        const Watching watching(snapshots);
        Library library(computer.library);
        QStringList said;
        library.onBringingUpToDate([&said](const QString &line) { said << line; });
        QVERIFY(library.open());
        QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
        QVERIFY2(said.join(u'\n').contains(u"aren't set up"_s), qPrintable(said.join(u'\n')));
        QCOMPARE(QDir(library.backupFolder()).entryList({u"omabook-format-12-*.db"_s}, QDir::Files).size(), 1);
    }

    void aRestoresCopyIsNoneOfTheLibraries() {
        const Computer computer;
        QVERIFY(computer.setUp());
        const QString copy = computer.state + u"/restoring/copy-1/library"_s;
        QVERIFY(olderLibrary(copy));
        const Snapshots snapshots = computer.snapshots();
        const Watching watching(snapshots);
        Library library(copy);
        QVERIFY(library.open());
        const auto all = Restic::snapshots(snapshots.place().repository, {u"omabook"_s});
        QVERIFY2(all, qPrintable(Fixtures::whyNot(all)));
        QVERIFY(all->isEmpty());
    }

    void theHubSnapshotsAnUpgradeFirst() {
        const Hub hub;
        QVERIFY(hub.setUp() && olderLibrary(hub.libraries + u"/home"_s));
        const Snapshots snapshots = hub.snapshots();
        HubKeeper keeper(hub.libraries);
        keeper.before(HubSnapshots::before(snapshots));
        const auto opened = keeper.open();
        QVERIFY2(opened, qPrintable(Fixtures::whyNot(opened)));
        QStringList found;
        QVERIFY(keeper.write(u"home"_s, HubKeeper::Moment::None, [&](Library &library) -> std::expected<QJsonObject, Failure> {
            found = reasons(snapshots, library);
            return QJsonObject{};
        }));
        QCOMPARE(found, QStringList{u"upgrade"_s});
    }

    void theHubRefusesAnUpgradeItCantSnapshot() {
        const Hub hub;
        const QString home = hub.libraries + u"/home"_s;
        QVERIFY(hub.setUp() && olderLibrary(home));
        QVERIFY(QDir(hub.dir.filePath(u"repository"_s)).removeRecursively());
        const Snapshots snapshots = hub.snapshots();
        // What the program did before an upgrade is what it does again once the keeper has opened.
        bool computers = false;
        const auto previous = LibraryMigrations::beforeUpgrading([&computers](Library &) -> std::expected<QString, Failure> {
            computers = true;
            return QString();
        });
        {
            HubKeeper keeper(hub.libraries);
            keeper.before(HubSnapshots::before(snapshots));
            const auto opened = keeper.open();
            const QString why = Fixtures::whyNot(opened);
            QVERIFY2(!opened && why.contains(u"left alone"_s), qPrintable(why));
        }
        QVERIFY(!computers);
        QCOMPARE(formatOf(Library::databaseIn(home)), 12);
        {
            Library elsewhere(hub.dir.filePath(u"elsewhere"_s));
            QVERIFY(olderLibrary(elsewhere.root()));
            QVERIFY(elsewhere.open());
        }
        LibraryMigrations::beforeUpgrading(previous);
        QVERIFY(computers);
    }

    void aBulkChangeIsSnapshotFirst() {
        const Computer computer;
        QVERIFY(computer.setUp());
        Library library(computer.library);
        QVERIFY(library.open() && entry(library, u"One"_s) > 0);
        const Snapshots snapshots = computer.snapshots();
        const auto first = MomentSnapshot::beforeBulk(snapshots, library);
        QVERIFY2(first, qPrintable(Fixtures::whyNot(first)));
        QVERIFY(!first->undo.isEmpty() && !first->unsnapshot);
        QCOMPARE(reasons(snapshots, library), QStringList{u"bulk-change"_s});
        // Not set up: nothing to undo with, and nothing in the way.
        const Computer bare;
        const auto none = MomentSnapshot::beforeBulk(bare.snapshots(), library);
        QVERIFY(none && none->undo.isEmpty() && !none->unsnapshot);
    }

    void aBulkChangeGoesOnWithAWarningOnAComputerAndIsRefusedOnTheHub() {
        const Computer computer;
        QVERIFY(computer.setUp());
        QVERIFY(QDir(computer.repository).removeRecursively());
        Library library(computer.library);
        QVERIFY(library.open() && entry(library, u"One"_s) > 0);
        const auto warned = MomentSnapshot::beforeBulk(computer.snapshots(), library);
        QVERIFY2(warned, qPrintable(Fixtures::whyNot(warned)));
        QVERIFY(warned->undo.isEmpty() && warned->unsnapshot);

        const Hub hub;
        QVERIFY(hub.setUp());
        QVERIFY(QDir(hub.dir.filePath(u"repository"_s)).removeRecursively());
        QVERIFY(!MomentSnapshot::beforeBulk(hub.snapshots(), library));
        // And the hub's own write of many books, in its keeper's lock, is refused before it begins.
        QVERIFY(QDir().mkpath(hub.libraries + u"/home"_s));
        {
            Library home(hub.libraries + u"/home"_s);
            QVERIFY(home.open() && entry(home, u"Kept"_s) > 0);
        }
        const Snapshots snapshots = hub.snapshots();
        HubKeeper keeper(hub.libraries);
        keeper.before(HubSnapshots::before(snapshots));
        QVERIFY(keeper.open());
        bool changed = false;
        const auto refused = keeper.write(u"home"_s, HubKeeper::Moment::Bulk, [&changed](Library &) -> std::expected<QJsonObject, Failure> {
            changed = true;
            return QJsonObject{};
        });
        QVERIFY(!refused && !changed);
    }

    void anAgentsChangeToManyBooksSaysItsUndo() {
        const Machine machine;
        const QString repository = machine.dir.filePath(u"repository"_s);
        const auto setUp = machine.omabook({u"snapshot"_s, u"setup"_s, u"--repository"_s, repository, u"--json"_s});
        QVERIFY2(setUp.code == 0, qPrintable(setUp.errors));
        Server server(machine);
        QJsonArray books;
        for (int i = 0; i <= MomentSnapshot::bulkBooks; ++i)
            books << server.data(u"book_add_entry"_s, {{u"title"_s, u"Book %1"_s.arg(i)}, {u"paper"_s, true}}).value(u"id"_s).toInteger();
        QVERIFY(!server.data(u"collection_add"_s, {{u"name"_s, u"Shelf"_s}}).isEmpty());

        // A few books: no snapshot.
        const QJsonObject few =
            server.data(u"collection_put"_s, {{u"name"_s, u"Shelf"_s}, {u"books"_s, QJsonArray{books.at(0)}}, {u"how"_s, u"include"_s}});
        QVERIFY(!few.isEmpty() && !few.contains(u"undo"_s));
        // Many: one first, said as the undo.
        const QJsonObject many = server.data(u"collection_put"_s, {{u"name"_s, u"Shelf"_s}, {u"books"_s, books}, {u"how"_s, u"include"_s}});
        const QString undo = many.value(u"undo"_s).toObject().value(u"snapshot"_s).toString();
        QVERIFY2(!undo.isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(many).toJson())));
        QVERIFY(many.value(u"undo"_s).toObject().value(u"how"_s).toString().contains(u"omabook snapshot restore"_s));
        const auto listed = machine.omabook({u"snapshot"_s, u"list"_s, u"--library"_s, machine.library, u"--json"_s});
        QVERIFY2(listed.code == 0, qPrintable(listed.errors));
        bool found = false;
        for (const QJsonValue &snapshot : QJsonDocument::fromJson(listed.output.toUtf8()).object().value(u"snapshots"_s).toArray())
            found = found
                    || (snapshot.toObject().value(u"id"_s).toString() == undo
                        && snapshot.toObject().value(u"reason"_s).toString() == u"bulk-change"_s);
        QVERIFY2(found, qPrintable(listed.output));

        // The repository gone: the change goes on here, and says it has no snapshot.
        QVERIFY(QDir(repository).removeRecursively());
        const QJsonObject unsnapshot =
            server.data(u"collection_put"_s, {{u"name"_s, u"Shelf"_s}, {u"books"_s, books}, {u"how"_s, u"exclude"_s}});
        QVERIFY(!unsnapshot.isEmpty() && !unsnapshot.contains(u"undo"_s));
        QVERIFY2(unsnapshot.value(u"notes"_s).toString().contains(u"No moment snapshot"_s),
                 qPrintable(QString::fromUtf8(QJsonDocument(unsnapshot).toJson())));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join(u'\n')));
    }

    void aCommandOnAnOlderLibrarySnapshotsItFirst() {
        const Machine machine;
        QVERIFY(olderLibrary(machine.library));
        const auto setUp = machine.omabook({u"snapshot"_s, u"setup"_s, u"--repository"_s, machine.dir.filePath(u"repository"_s)});
        QVERIFY2(setUp.code == 0, qPrintable(setUp.errors));
        const auto listing = machine.omabook({u"list"_s, u"--library"_s, machine.library});
        QVERIFY2(listing.code == 0 && listing.errors.contains(u"moment snapshot"_s), qPrintable(listing.errors));
        const auto listed = machine.omabook({u"snapshot"_s, u"list"_s, u"--library"_s, machine.library, u"--json"_s});
        const QJsonArray snapshots = QJsonDocument::fromJson(listed.output.toUtf8()).object().value(u"snapshots"_s).toArray();
        QVERIFY2(snapshots.size() == 1, qPrintable(listed.output + listed.errors));
        QCOMPARE(snapshots.at(0).toObject().value(u"reason"_s).toString(), u"upgrade"_s);
    }
};

QTEST_MAIN(MomentSnapshotTests)
#include "momentsnapshot_tests.moc"
