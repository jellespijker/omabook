// Snapshots of a library with restic (#181, ADR 0014), in a repository in a
// folder of the test's own, never the user's library or home: set up with a
// recovery key shown once, consistent while another writes (the restored
// copy passes SQLite's integrity check, and holds whole batches only),
// holding the books, the settings and the plugins' data but no caches,
// taken as a moment snapshot only when something changed, thinned as the
// retention says, and taken by a risky command before it changes anything.

#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <thread>
#include <tuple>
#include <utility>

#include "bookmeta.h"
#include "fixtures.h"
#include "library.h"
#include "snapshotcommands.h"
#include "snapshots.h"

using namespace std::chrono_literals;

namespace {

[[nodiscard]] qint64 entry(Library &library, const QString &title) {
    BookMeta meta;
    meta.title = title;
    return library.addEntry(meta, true).id;
}

// A computer of the test's own: its snapshots' state, its repository, its
// settings and plugins, and a library.
struct Computer {
    QTemporaryDir dir;
    QString library = dir.filePath("Library");
    QString state = dir.filePath("state/omabook/snapshots");
    QString repository = dir.filePath("repository");
    SnapshotContents contents{.settingsFile = dir.filePath("config/omabook/config.json"),
                              .pluginsFolder = dir.filePath("config/omabook/plugins")};

    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::in(state), contents); }

    // The recovery key, and the snapshots set up with it.
    [[nodiscard]] QString setUp() const {
        Snapshots made = snapshots();
        const auto key = made.setUp(repository);
        return key ? *key : QString();
    }

    // The snapshot `id` restored with the recovery key alone, into a folder of its own.
    [[nodiscard]] QString restore(const QString &id, const QString &key) const {
        const QString keyFile = dir.filePath("typed-key"), target = dir.filePath("restored-" + id.left(8));
        if (!Fixtures::write(keyFile, key.toUtf8()))
            return {};
        QProcess restic;
        restic.start("restic", {"--repo", repository, "--password-file", keyFile, "restore", id, "--target", target});
        return restic.waitForFinished(60000) && restic.exitCode() == 0 ? target : QString();
    }

    // The database's copy in a restored snapshot.
    [[nodiscard]] QString restoredDatabase(const QString &target, const Library &of) const {
        return target + state + "/staging/" + Snapshots::libraryTag(of).section(':', 1) + "/omabook.db";
    }
};

[[nodiscard]] QString integrity(const QString &database) {
    QString answer;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "restored");
        db.setDatabaseName(database);
        if (db.open()) {
            QSqlQuery check(db);
            if (check.exec("PRAGMA integrity_check") && check.next())
                answer = check.value(0).toString();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("restored");
    return answer;
}

// The books of the library in `folder`, opened as it is.
[[nodiscard]] QStringList booksOf(const QString &folder) {
    Library library(folder);
    library.keepFormat();
    if (!library.open())
        return {"(not opened)"};
    QStringList books;
    for (const Book &book : library.books())
        books << book.meta.title + " by " + book.meta.authors.join(" & ");
    books.sort();
    return books;
}

// The books of a restored database, put in a library folder of its own.
[[nodiscard]] QStringList booksIn(const QString &database, const QString &folder) {
    if (!QDir().mkpath(folder) || !QFile::copy(database, Library::databaseIn(folder)))
        return {"(not copied)"};
    return booksOf(folder);
}

} // namespace

class SnapshotsTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY2(!QStandardPaths::findExecutable("restic").isEmpty(), "restic is one of Omabook's dependencies"); }

    void settingUpShowsTheRecoveryKeyOnceAndKeepsThePasswordPrivate() {
        const Computer computer;
        QVERIFY(!computer.snapshots().ready());
        const QString key = computer.setUp();
        QVERIFY2(QRegularExpression("^([A-HJ-NP-Z2-9]{4}-){7}[A-HJ-NP-Z2-9]{4}$").match(key).hasMatch(), qPrintable(key));
        Snapshots again = computer.snapshots();
        QVERIFY(again.ready());
        const QFile::Permissions password = QFile::permissions(computer.state + "/password");
        QVERIFY(password.testFlag(QFile::ReadOwner));
        QVERIFY(!(password & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
        QVERIFY(QFileInfo::exists(computer.repository + "/config"));
        // Never shown again: setting up twice is refused.
        const auto twice = again.setUp(computer.repository);
        QVERIFY(!twice);
        QCOMPARE(twice.error().code(), Failure::Code::SnapshotsSetUpAlready);
        // Not set up, nothing is taken before a risky moment, and nothing fails.
        const Computer other;
        Library library(other.library);
        QVERIFY(library.open());
        const auto none = other.snapshots().before(library, SnapshotReason::Sync);
        QVERIFY2(none && !*none, qPrintable(Fixtures::whyNot(none)));
    }

    void aSnapshotTakenWhileAnotherWritesIsWhole() {
        const Computer computer;
        const QString key = computer.setUp();
        QVERIFY(!key.isEmpty());
        Library library(computer.library);
        QVERIFY(library.open());
        // Another writer adds two books at a time, batch after batch.
        std::atomic<bool> done = false;
        std::atomic<int> batches = 0;
        const QString root = computer.library;
        std::jthread writer([&root, &done, &batches] {
            Library other(root);
            if (!other.open())
                return;
            for (int at = 0; !done; ++at) {
                const Library::Batch batch(other);
                std::ignore = entry(other, QString("Left %1").arg(at));
                std::this_thread::sleep_for(2ms);
                std::ignore = entry(other, QString("Right %1").arg(at));
                ++batches;
            }
        });
        QTRY_VERIFY(batches.load() > 2);
        QStringList taken;
        for (int i = 0; i < 3; ++i) {
            const auto snapshot = computer.snapshots().take(library, SnapshotReason::Asked);
            QVERIFY2(snapshot, qPrintable(Fixtures::whyNot(snapshot)));
            taken << snapshot->id;
        }
        done = true;
        writer.join();
        for (const QString &id : taken) {
            const QString target = computer.restore(id, key);
            QVERIFY2(!target.isEmpty(), qPrintable(id));
            const QString database = computer.restoredDatabase(target, library);
            QCOMPARE(integrity(database), QString("ok"));
            const QStringList books = booksIn(database, target + "-library");
            QVERIFY2(!books.isEmpty() && books.size() % 2 == 0, qPrintable(books.join(", ")));
            // The library's own database is kept only as its copy.
            QVERIFY(!QFileInfo::exists(target + computer.library + "/omabook.db"));
        }
    }

    void aSnapshotHoldsTheBooksTheSettingsAndThePluginsDataButNoCaches() {
        const Computer computer;
        const QString key = computer.setUp();
        Library library(computer.library);
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(computer.dir.filePath("book.epub"), "A Kept Book"));
        const auto added = library.add(computer.dir.filePath("book.epub"));
        QVERIFY2(added.error.isEmpty(), qPrintable(added.error));
        QVERIFY(QDir().mkpath(computer.contents.pluginsFolder + "/notebooks/data/pages"));
        QVERIFY(Fixtures::write(computer.contents.settingsFile, "{\"font_size\": 21}"));
        QVERIFY(Fixtures::write(computer.contents.pluginsFolder + "/notebooks/data/pages/one.json", "{\"ink\": []}"));
        QVERIFY(Fixtures::write(computer.contents.pluginsFolder + "/notebooks/main.js", "// installed again, not kept"));
        QVERIFY(QDir().mkpath(library.backupFolder()));
        QVERIFY(Fixtures::write(library.backupFolder() + "/omabook-format-1-20260101-000000.db", "an older format"));

        const auto taken = computer.snapshots().take(library, SnapshotReason::Asked);
        QVERIFY2(taken, qPrintable(Fixtures::whyNot(taken)));
        const QString target = computer.restore(taken->id, key);
        QVERIFY(!target.isEmpty());
        QVERIFY(QFileInfo::exists(computer.restoredDatabase(target, library)));
        const QString book = target + computer.library + "/" + library.book(added.id)->path;
        QVERIFY2(QDir(book).entryList({"*.epub"}, QDir::Files).size() == 1, qPrintable(book));
        QVERIFY(QFileInfo::exists(target + computer.contents.settingsFile));
        QVERIFY(QFileInfo::exists(target + computer.contents.pluginsFolder + "/notebooks/data/pages/one.json"));
        QVERIFY(!QFileInfo::exists(target + computer.contents.pluginsFolder + "/notebooks/main.js"));
        QVERIFY(!QFileInfo::exists(target + library.backupFolder()));
        // Where each came from, for a restore to put it back.
        QFile manifest(QFileInfo(computer.restoredDatabase(target, library)).absolutePath() + "/snapshot.json");
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(manifest.readAll()).object().value("library").toString(), library.root());
    }

    void aMomentSnapshotIsTakenOnlyWhenSomethingChanged() {
        const Computer computer;
        QVERIFY(!computer.setUp().isEmpty());
        Library library(computer.library);
        QVERIFY(library.open());
        QVERIFY(entry(library, "Before") > 0);
        const Snapshots snapshots = computer.snapshots();
        const auto first = snapshots.before(library, SnapshotReason::Sync);
        QVERIFY2(first && *first && !(*first)->unchanged, qPrintable(Fixtures::whyNot(first)));
        const auto again = snapshots.before(library, SnapshotReason::Import);
        QVERIFY2(again && *again, qPrintable(Fixtures::whyNot(again)));
        QVERIFY((*again)->unchanged);
        QCOMPARE((*again)->id, (*first)->id);
        QVERIFY(entry(library, "After") > 0);
        const auto changed = snapshots.before(library, SnapshotReason::Tidy);
        QVERIFY2(changed && *changed && !(*changed)->unchanged, qPrintable(Fixtures::whyNot(changed)));
        QVERIFY((*changed)->id != (*first)->id);
        // The nightly one is taken whatever changed, once a night.
        const auto nightly = snapshots.nightly(library, QDateTime::currentDateTime());
        QVERIFY2(nightly && *nightly && !(*nightly)->unchanged, qPrintable(Fixtures::whyNot(nightly)));
        const auto notAgain = snapshots.nightly(library, QDateTime::currentDateTime());
        QVERIFY2(notAgain && !*notAgain, qPrintable(Fixtures::whyNot(notAgain)));

        const auto listed = snapshots.list(library);
        QVERIFY2(listed, qPrintable(Fixtures::whyNot(listed)));
        QCOMPARE(listed->size(), 3);
        QCOMPARE(listed->at(0).tagged("reason:"), QString("sync"));
        QVERIFY(listed->at(0).tags.contains("moment"));
        QCOMPARE(listed->at(1).tagged("reason:"), QString("tidy"));
        QVERIFY(listed->at(2).tags.contains("nightly"));
    }

    void thinningForgetsWhatIsKeptNoLonger() {
        const Computer computer;
        QVERIFY(!computer.setUp().isEmpty());
        Library library(computer.library);
        QVERIFY(library.open());
        const Snapshots snapshots = computer.snapshots();
        for (const QString &title : {"One", "Two", "Three"}) {
            QVERIFY(entry(library, title) > 0);
            const auto taken = snapshots.take(library, SnapshotReason::Asked);
            QVERIFY2(taken, qPrintable(Fixtures::whyNot(taken)));
        }
        const auto before = snapshots.list(library);
        QVERIFY(before && before->size() == 3);
        const auto none = snapshots.thin(library, QDateTime::currentDateTime());
        QVERIFY2(none && *none == 0, qPrintable(Fixtures::whyNot(none)));
        // Two months on, the one of that day is left: the newest.
        const auto thinned = snapshots.thin(library, QDateTime::currentDateTime().addDays(60));
        QVERIFY2(thinned, qPrintable(Fixtures::whyNot(thinned)));
        QCOMPARE(*thinned, 2);
        const auto after = snapshots.list(library);
        QVERIFY(after && after->size() == 1);
        QCOMPARE(after->first().id, before->last().id);
    }

    void theRiskyCommandsAreKnown() {
        const auto of = [](const QStringList &words) { return riskyMoment(QStringList{"omabook"} + words); };
        QVERIFY(of({"sync", "laptop"}) == SnapshotReason::Sync);
        QVERIFY(of({"sync", "--library", "/books", "laptop:Books"}) == SnapshotReason::Sync);
        QVERIFY(of({"spaces", "sync"}) == SnapshotReason::Sync);
        QVERIFY(of({"tidy", "--apply"}) == SnapshotReason::Tidy);
        QVERIFY(of({"tidy", "--pick", "1,3"}) == SnapshotReason::Tidy);
        QVERIFY(of({"calibre", "import", "--all"}) == SnapshotReason::Import);
        QVERIFY(of({"goodreads", "import", "shelves.csv"}) == SnapshotReason::Import);
        QVERIFY(of({"goodreads", "--user", "import", "sync"}) == SnapshotReason::Import);
        QVERIFY(of({"openlibrary", "sync"}) == SnapshotReason::Import);
        QVERIFY(of({"opds", "https://standardebooks.org/feeds/opds", "--add", "3"}) == SnapshotReason::Import);
        // A look changes nothing.
        for (const QStringList &look : QList<QStringList>{{"tidy"},
                                                          {"sync", "--status"},
                                                          {"sync", "laptop", "--dry-run"},
                                                          {"calibre", "libraries"},
                                                          {"goodreads", "export", "out.csv"},
                                                          {"opds", "https://standardebooks.org/feeds/opds"},
                                                          {"sync", "--help"},
                                                          {"list"},
                                                          {"spaces"}})
            QVERIFY2(!of(look), qPrintable(look.join(' ')));
    }

    void aRiskyCommandTakesAMomentSnapshotFirst() {
        QTemporaryDir dir;
        const QString library = dir.filePath("Library"), repository = dir.filePath("repository");
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const auto &[name, folder] : {std::pair{"HOME", "home"},
                                           {"XDG_CONFIG_HOME", "home/.config"},
                                           {"XDG_CACHE_HOME", "home/.cache"},
                                           {"XDG_DATA_HOME", "home/.local/share"},
                                           {"XDG_STATE_HOME", "home/.local/state"}})
            environment.insert(name, dir.filePath(folder));
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.remove("CREDENTIALS_DIRECTORY");
        QVERIFY(QDir().mkpath(dir.filePath("home/.config/omabook")));
        QVERIFY(Fixtures::write(dir.filePath("home/.config/omabook/config.json"), "{\"ai_provider\": \"off\"}"));
        const auto omabook = [&environment](const QStringList &arguments) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(OMABOOK_BINARY, arguments);
            process.closeWriteChannel();
            process.waitForFinished(120000);
            return std::pair{process.exitCode(), QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
        };
        const auto [setUp, said] = omabook({"snapshot", "setup", "--repository", repository, "--json"});
        QVERIFY2(setUp == 0, qPrintable(said));
        const QString key = QJsonDocument::fromJson(said.toUtf8()).object().value("recovery_key").toString();
        QVERIFY2(!key.isEmpty(), qPrintable(said));
        // The same book twice, its author spelled two ways: tidy merges them.
        QVERIFY(Fixtures::write(dir.filePath("Twice - Reader, Rita.txt"), "One copy.\n"));
        QVERIFY(Fixtures::write(dir.filePath("twice - Rita Reader.md"), "Another copy.\n"));
        QCOMPARE(
            omabook({"add", "--library", library, dir.filePath("Twice - Reader, Rita.txt"), dir.filePath("twice - Rita Reader.md")}).first,
            0);
        const QStringList before = booksOf(library);
        QCOMPARE(before.size(), 2);
        // Listing what would be tidied changes nothing, and takes none.
        QCOMPARE(omabook({"tidy", "--library", library}).first, 0);
        const auto [tidied, tidySaid] = omabook({"tidy", "--apply", "--library", library});
        QVERIFY2(tidied == 0 && tidySaid.contains("moment snapshot"), qPrintable(tidySaid));
        QVERIFY(booksOf(library) != before);

        const auto [listed, list] = omabook({"snapshot", "list", "--library", library, "--json"});
        QVERIFY2(listed == 0, qPrintable(list));
        const QJsonArray snapshots = QJsonDocument::fromJson(list.toUtf8()).object().value("snapshots").toArray();
        QCOMPARE(snapshots.size(), 1);
        QCOMPARE(snapshots.at(0).toObject().value("kind").toString(), QString("moment"));
        QCOMPARE(snapshots.at(0).toObject().value("reason").toString(), QString("tidy"));
        // It holds the library as it was before the tidy.
        const QString keyFile = dir.filePath("key"), target = dir.filePath("restored");
        QVERIFY(Fixtures::write(keyFile, key.toUtf8()));
        QProcess restic;
        restic.start("restic", {"--repo", repository, "--password-file", keyFile, "restore",
                                snapshots.at(0).toObject().value("id").toString(), "--target", target});
        QVERIFY(restic.waitForFinished(60000) && restic.exitCode() == 0);
        QString database;
        for (QDirIterator found(target, {"omabook.db"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); found.hasNext();)
            database = found.next();
        QVERIFY(!database.isEmpty());
        QCOMPARE(booksIn(database, dir.filePath("restored-library")), before);
    }
};

QTEST_MAIN(SnapshotsTests)
#include "snapshots_tests.moc"
