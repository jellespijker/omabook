// Going back to a snapshot (#183, ADR 0014): the library diff between two
// snapshots and between one and now; the whole library restored into a new
// folder, never over the live one; and one book's details or annotations put
// back into the live library after a moment snapshot that undoes it, by the
// command (which shows the diff and asks first) and by a hub's keeper. In a
// repository and libraries of the test's own, never the user's.

#include <QDir>
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
#include "hubkeeper.h"
#include "hubplace.h"
#include "hubrequests.h"
#include "hubsnapshots.h"
#include "library.h"
#include "plugindata.h"
#include "snapshotrestore.h"
#include "snapshots.h"
#include "spaces.h"

using Fixtures::whyNot;

namespace {

[[nodiscard]] qint64 entry(Library &library, const QString &title) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Ann Author"};
    return library.addEntry(meta, true).id;
}

[[nodiscard]] QStringList booksOf(const Library &library) {
    QStringList books;
    for (const Book &book : library.books())
        books << book.meta.title;
    books.sort();
    return books;
}

[[nodiscard]] bool rename(Library &library, qint64 id, const QString &title) {
    BookMeta meta = library.book(id)->meta;
    meta.title = title;
    return library.setMeta(id, meta);
}

// A computer of the test's own: home, settings and snapshots, and its library.
struct Computer {
    QTemporaryDir dir;
    QString library = dir.filePath("Library");
    QString repository = dir.filePath("repository");
    QString state = dir.filePath("home/.local/state/omabook/snapshots");
    SnapshotContents contents{.settingsFile = dir.filePath("home/.config/omabook/config.json"),
                              .pluginsFolder = dir.filePath("home/.config/omabook/plugins")};

    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::in(state), contents); }
    [[nodiscard]] bool setUp() const {
        Snapshots made = snapshots();
        return made.setUp(repository).has_value();
    }

    // This build's omabook, run as this computer's: how it ended, all it said, and what of that on standard output.
    struct Ran {
        int code = 0;
        QString said, out;
    };
    [[nodiscard]] Ran omabook(const QStringList &arguments, const QByteArray &typed = {}) const {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const auto &[name, folder] : {std::pair{"HOME", "home"},
                                           {"XDG_CONFIG_HOME", "home/.config"},
                                           {"XDG_CACHE_HOME", "home/.cache"},
                                           {"XDG_DATA_HOME", "home/.local/share"},
                                           {"XDG_STATE_HOME", "home/.local/state"}})
            environment.insert(name, dir.filePath(folder));
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.remove("CREDENTIALS_DIRECTORY");
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(OMABOOK_BINARY, arguments);
        process.write(typed);
        process.closeWriteChannel();
        process.waitForFinished(120000);
        const QByteArray out = process.readAllStandardOutput();
        return {.code = process.exitCode(), .said = QString::fromUtf8(out + process.readAllStandardError()), .out = QString::fromUtf8(out)};
    }
};

[[nodiscard]] QString taken(const Snapshots &snapshots, const Library &library) {
    const auto made = snapshots.take(library, SnapshotReason::Asked);
    return made ? made->id : QString();
}

} // namespace

class SnapshotRestoreTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY2(!QStandardPaths::findExecutable("restic").isEmpty(), "restic is one of Omabook's dependencies"); }

    // Two snapshots and now, three moments: each diff tells what came between.
    void aDiffBetweenTwoSnapshotsOrASnapshotAndNow() {
        const Computer computer;
        QVERIFY(QDir().mkpath(computer.dir.filePath("home/.config/omabook")));
        QVERIFY(Fixtures::write(computer.contents.settingsFile, "{\"ai_provider\": \"off\"}"));
        QVERIFY(computer.setUp());
        const Snapshots snapshots = computer.snapshots();
        Library library(computer.library);
        QVERIFY(library.open());
        const qint64 dune = entry(library, "Dune");
        const QString first = taken(snapshots, library);
        QVERIFY(!first.isEmpty());
        QVERIFY(entry(library, "Emma") > 0);
        const QString second = taken(snapshots, library);
        QVERIFY(!second.isEmpty());
        QVERIFY(rename(library, dune, "Dune Messiah"));

        const auto found = SnapshotRestore::find(snapshots, library, first.left(8));
        QVERIFY2(found, qPrintable(whyNot(found)));
        QCOMPARE(found->id, first);
        const auto copy = SnapshotRestore::Copy::open(snapshots, *found);
        QVERIFY2(copy, qPrintable(whyNot(copy)));
        QCOMPARE(booksOf(copy->library()), QStringList{"Dune"});
        const auto later = SnapshotRestore::Copy::open(snapshots, *SnapshotRestore::find(snapshots, library, second));
        QVERIFY(later);
        const LibraryDiff::Diff between = LibraryDiff::between(copy->library(), later->library());
        QCOMPARE(between.added.size(), 1);
        QCOMPARE(between.added.first().title, QString("Emma"));
        QVERIFY(between.changed.isEmpty());
        const LibraryDiff::Diff sinceFirst = LibraryDiff::between(copy->library(), library);
        QCOMPARE(sinceFirst.added.size(), 1);
        QCOMPARE(sinceFirst.changed.size(), 1);
        QCOMPARE(sinceFirst.changed.first().details.first().before, QString("Dune"));
        // No such snapshot of this library.
        const auto none = SnapshotRestore::find(snapshots, library, "ffffffff");
        QVERIFY(!none);
        QCOMPARE(none.error().code(), Failure::Code::SnapshotNotFound);

        // The command: in text, and as JSON.
        const auto [code, said, out] = computer.omabook({"snapshot", "diff", first.left(8), "--library", computer.library});
        QVERIFY2(code == 0, qPrintable(said));
        QVERIFY2(said.contains("Emma") && said.contains("Dune Messiah") && said.contains("title"), qPrintable(said));
        const auto [jsonCode, saidJson, json] =
            computer.omabook({"snapshot", "diff", first, second, "--library", computer.library, "--json"});
        QVERIFY2(jsonCode == 0, qPrintable(saidJson));
        const QJsonObject parsed = QJsonDocument::fromJson(json.toUtf8()).object();
        QCOMPARE(parsed["from"]["id"].toString(), first);
        QCOMPARE(parsed["to"]["id"].toString(), second);
        QCOMPARE(parsed["books"]["added"].toArray().size(), 1);
        QCOMPARE(parsed["books"]["changed"].toArray().size(), 0);
    }

    // Into a new folder, the books' files with it; the live library untouched,
    // and never restored over, nor into a folder that holds something.
    void aWholeLibraryIsRestoredIntoANewFolderOnly() {
        const Computer computer;
        QVERIFY(computer.setUp());
        const Snapshots snapshots = computer.snapshots();
        Library library(computer.library);
        QVERIFY(library.open());
        QVERIFY(Fixtures::write(computer.dir.filePath("Ferry.txt"), "The harbour master counted the crates.\n"));
        QVERIFY(library.add(computer.dir.filePath("Ferry.txt")).id > 0);
        QVERIFY(entry(library, "Dune") > 0);
        const auto snapshot = SnapshotRestore::find(snapshots, library, taken(snapshots, library));
        QVERIFY(snapshot);
        QVERIFY(entry(library, "Emma") > 0);

        const QString folder = computer.dir.filePath("Restored");
        const auto restored = SnapshotRestore::whole(snapshots, *snapshot, folder);
        QVERIFY2(restored, qPrintable(whyNot(restored)));
        Library back(folder);
        QVERIFY(back.open());
        QCOMPARE(booksOf(back).size(), 2);
        const QString ferry = back.formatPath(back.books("Ferry").first().id);
        QVERIFY2(QFileInfo::exists(ferry), qPrintable(ferry));
        QCOMPARE(booksOf(library).size(), 3);

        for (const QString &refused : {folder, computer.library, computer.library + "/Inside"}) {
            const auto again = SnapshotRestore::whole(snapshots, *snapshot, refused);
            QVERIFY2(!again, qPrintable(refused));
            QCOMPARE(again.error().code(), Failure::Code::RestoreFolderTaken);
        }
        QCOMPARE(booksOf(library).size(), 3);
        // By the command too.
        const auto [code, said, out] =
            computer.omabook({"snapshot", "restore", snapshot->id, "--to", computer.dir.filePath("Again"), "--library", computer.library});
        QVERIFY2(code == 0, qPrintable(said));
        QVERIFY(QFileInfo::exists(Library::databaseIn(computer.dir.filePath("Again"))));
    }

    // A restored library is a library of its own, never the one it was a
    // snapshot of speaking twice (#209): it is given a device id of its own,
    // and the command says so.
    void aRestoredLibraryHasADeviceIdOfItsOwn() {
        const Computer computer;
        QVERIFY(computer.setUp());
        const Snapshots snapshots = computer.snapshots();
        Library library(computer.library);
        QVERIFY(library.open());
        QVERIFY(entry(library, "Dune") > 0);
        const QString was = Spaces::deviceId(library);
        const auto snapshot = SnapshotRestore::find(snapshots, library, taken(snapshots, library));
        QVERIFY(snapshot);

        const QString folder = computer.dir.filePath("Restored");
        const auto restored = SnapshotRestore::whole(snapshots, *snapshot, folder);
        QVERIFY2(restored, qPrintable(whyNot(restored)));
        QCOMPARE(restored->was, was);
        Library back(folder);
        QVERIFY(back.open());
        QCOMPARE(Spaces::deviceId(back), restored->now);
        QVERIFY(restored->now != was);
        QCOMPARE(Spaces::deviceId(library), was);

        const auto [code, said, out] = computer.omabook(
            {"snapshot", "restore", snapshot->id, "--to", computer.dir.filePath("Again"), "--library", computer.library, "--json"});
        QVERIFY2(code == 0, qPrintable(said));
        const QJsonObject answered = QJsonDocument::fromJson(out.toUtf8()).object();
        Library again(computer.dir.filePath("Again"));
        QVERIFY(again.open());
        QCOMPARE(answered["device"].toString(), Spaces::deviceId(again));
        QCOMPARE(answered["was"].toString(), was);
        const auto [textCode, saidText, outText] =
            computer.omabook({"snapshot", "restore", snapshot->id, "--to", computer.dir.filePath("Third"), "--library", computer.library});
        QVERIFY2(textCode == 0 && saidText.contains("device id of its own"), qPrintable(saidText));
    }

    // A notebook of the notebooks plugin, and a page of it, put back by the
    // command from a snapshot (#209): shown first and asked, a moment
    // snapshot taken, the notebook's page removed since back, and its title.
    void aNotebookOrAPageIsPutBackByTheCommand() {
        const Computer computer;
        QVERIFY(QDir().mkpath(computer.dir.filePath("home/.config/omabook")));
        QVERIFY(Fixtures::write(computer.contents.settingsFile, "{\"ai_provider\": \"off\"}"));
        QVERIFY(computer.setUp());
        const auto thing = [](const QString &entity, const QString &id) {
            return PluginData::Thing{.plugin = "notebooks", .entity = entity, .id = id};
        };
        QString before;
        {
            const Snapshots snapshots = computer.snapshots();
            Library library(computer.library);
            QVERIFY(library.open());
            QVERIFY(PluginData::set(library, thing("notebook", "n1"), {{"title", "Physics"}}));
            QVERIFY(PluginData::set(library, thing("page", "p1"), {{"notebook", "n1"}, {"order", 1}}));
            QVERIFY(PluginData::change(library, thing("page", "p1"), "strokes", {.added = {{"s1", "AAA"}}, .removed = {}}));
            QVERIFY(PluginData::set(library, thing("page", "p2"), {{"notebook", "n1"}, {"order", 2}}));
            before = taken(snapshots, library);
            QVERIFY(!before.isEmpty());
            QVERIFY(PluginData::set(library, thing("notebook", "n1"), {{"title", "Chemistry"}}));
            QVERIFY(PluginData::remove(library, thing("page", "p2")));
            QVERIFY(PluginData::change(library, thing("page", "p1"), "strokes", {.added = {}, .removed = {"s1"}}));
        }
        const auto now = [&computer, &thing](const QString &entity, const QString &id) {
            Library library(computer.library);
            return library.open() ? PluginData::read(library, thing(entity, id)).toObject() : QJsonObject();
        };
        const QStringList page{"snapshot", "restore", before.left(8), "--page", "p1", "--yes", "--library", computer.library};
        const auto [pageCode, saidPage, outPage] = computer.omabook(page);
        QVERIFY2(pageCode == 0, qPrintable(saidPage));
        QCOMPARE(now("page", "p1")["sets"].toObject()["strokes"].toObject().keys(), QStringList{"s1"});
        QCOMPARE(now("notebook", "n1")["fields"].toObject()["title"].toString(), QString("Chemistry"));

        const QStringList notebook{"snapshot", "restore", before.left(8), "--notebook", "Physics", "--library", computer.library};
        const auto [no, saidNo, outNo] = computer.omabook(notebook, "n\n");
        QVERIFY2(no != 0 && saidNo.contains("page"), qPrintable(saidNo));
        QVERIFY(now("page", "p2").isEmpty());
        const auto [yes, saidYes, outYes] = computer.omabook(notebook + QStringList{"--json"}, "y\n");
        QVERIFY2(yes == 0, qPrintable(saidYes));
        QCOMPARE(now("notebook", "n1")["fields"].toObject()["title"].toString(), QString("Physics"));
        QCOMPARE(now("page", "p2")["fields"].toObject()["notebook"].toString(), QString("n1"));
        const QJsonObject answered = QJsonDocument::fromJson(outYes.toUtf8()).object();
        QVERIFY2(!answered["undo"].toString().isEmpty() && answered["undo"].toString() != before, qPrintable(outYes));
        QVERIFY(answered["restored"]["things"].toInt() >= 2);
        // A notebook the snapshot doesn't hold.
        const auto [none, saidNone, outNone] =
            computer.omabook({"snapshot", "restore", before.left(8), "--notebook", "Poems", "--yes", "--library", computer.library});
        QVERIFY2(none != 0 && saidNone.contains("Poems"), qPrintable(saidNone));
    }

    // The command shows what the restore puts back and asks; said no, or not
    // asked, nothing changes. Said yes, a moment snapshot first, and the
    // restore of that one undoes it.
    void aBookRestoreShowsTheDiffAsksAndCanBeUndone() {
        const Computer computer;
        QVERIFY(QDir().mkpath(computer.dir.filePath("home/.config/omabook")));
        QVERIFY(Fixtures::write(computer.contents.settingsFile, "{\"ai_provider\": \"off\"}"));
        QVERIFY(computer.setUp());
        QString gid, before;
        qint64 dune = 0;
        {
            const Snapshots snapshots = computer.snapshots();
            Library library(computer.library);
            QVERIFY(library.open());
            dune = entry(library, "Dune");
            gid = Spaces::bookId(library, dune);
            before = taken(snapshots, library);
            QVERIFY(rename(library, dune, "Dark Ages: Tremere"));
        }
        const QStringList restore{"snapshot", "restore", before.left(8), "--book", gid, "--details", "--library", computer.library};
        const auto titleNow = [&computer, dune] {
            Library library(computer.library);
            return library.open() ? library.book(dune)->meta.title : QString();
        };
        const auto [no, saidNo, outNo] = computer.omabook(restore, "n\n");
        QVERIFY2(no != 0 && saidNo.contains("Dark Ages: Tremere") && saidNo.contains("Dune"), qPrintable(saidNo));
        QCOMPARE(titleNow(), QString("Dark Ages: Tremere"));
        const auto [silent, saidSilent, outSilent] = computer.omabook(restore);
        QVERIFY2(silent != 0, qPrintable(saidSilent));
        QCOMPARE(titleNow(), QString("Dark Ages: Tremere"));

        const auto [yes, saidYes, outYes] = computer.omabook(restore + QStringList{"--json"}, "y\n");
        QVERIFY2(yes == 0, qPrintable(saidYes));
        QCOMPARE(titleNow(), QString("Dune"));
        const QJsonObject answered = QJsonDocument::fromJson(outYes.toUtf8()).object();
        const QString undo = answered["undo"].toString();
        QVERIFY2(!undo.isEmpty() && undo != before, qPrintable(saidYes));
        QVERIFY(answered["restored"]["details"].toBool());

        const auto [undone, saidUndone, outUndone] =
            computer.omabook({"snapshot", "restore", undo, "--book", "Dune", "--details", "--yes", "--library", computer.library});
        QVERIFY2(undone == 0, qPrintable(saidUndone));
        QCOMPARE(titleNow(), QString("Dark Ages: Tremere"));
    }

    // On a hub, through its keeper: the diff read and the book put back in
    // its lock, with the keeper's moment snapshot just before.
    void aHubRestoresABookThroughItsKeeper() {
        QTemporaryDir dir;
        const QString libraries = dir.filePath("libraries");
        const HubPlace place{{.state = dir.filePath("state/omabook-hub"), .runtime = dir.filePath("runtime"), .credentials = {}}};
        const SnapshotContents contents{.settingsFile = dir.filePath("config.json"), .pluginsFolder = dir.filePath("plugins")};
        Snapshots made(SnapshotPlace::ofHub(place), contents);
        QVERIFY(made.setUp(dir.filePath("repository")));
        const Snapshots snapshots(SnapshotPlace::ofHub(place), contents);
        QString gid, before;
        {
            Library home(QDir(libraries).filePath("home"));
            QVERIFY(home.open());
            const qint64 dune = entry(home, "Dune");
            gid = Spaces::bookId(home, dune);
            before = taken(snapshots, home);
            QVERIFY(rename(home, dune, "Dark Ages: Tremere"));
        }
        HubKeeper keeper(libraries);
        keeper.before(HubSnapshots::before(snapshots));
        QVERIFY(keeper.open());
        HubRequests requests;
        HubSnapshots::answer(requests, snapshots);

        const QJsonObject listed = requests.answer({{"request", "snapshots"}, {"library", "home"}}, keeper);
        QVERIFY2(listed["ok"].toBool(), qPrintable(listed["error"].toString()));
        QCOMPARE(listed["snapshots"].toArray().size(), 1);
        const QJsonObject diff = requests.answer({{"request", "snapshot-diff"}, {"library", "home"}, {"from", before}}, keeper);
        QVERIFY2(diff["ok"].toBool(), qPrintable(diff["error"].toString()));
        QCOMPARE(diff["books"]["changed"].toArray().first()["details"].toArray().first()["before"].toString(), QString("Dune"));
        const QJsonObject restored = requests.answer(
            {{"request", "snapshot-restore"}, {"library", "home"}, {"snapshot", before}, {"book", gid}, {"parts", QJsonArray{"details"}}},
            keeper);
        QVERIFY2(restored["ok"].toBool(), qPrintable(restored["error"].toString()));
        QVERIFY(restored["restored"]["details"].toBool());
        const QString undo = restored["undo"].toString();
        QVERIFY(!undo.isEmpty() && undo != before);
        const QJsonObject after = requests.answer({{"request", "snapshots"}, {"library", "home"}}, keeper);
        QCOMPARE(after["snapshots"].toArray().last()["reason"].toString(), QString("restore"));
        QVERIFY(keeper.write("home", HubKeeper::Moment::None, [](Library &library) -> std::expected<QJsonObject, Failure> {
            if (library.books().first().meta.title != "Dune")
                return std::unexpected(Failure::said(library.books().first().meta.title));
            return QJsonObject{};
        }));
        // Asked for no part, or a snapshot of no such id, nothing is done.
        const QJsonObject nothing =
            requests.answer({{"request", "snapshot-restore"}, {"library", "home"}, {"snapshot", before}, {"book", gid}}, keeper);
        QVERIFY(!nothing["ok"].toBool());
    }
};

QTEST_GUILESS_MAIN(SnapshotRestoreTests)
#include "snapshotrestore_tests.moc"
