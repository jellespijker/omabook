// A book in or out of a collection by hand, through syncs with a computer
// that took it out or put it in (#300).
#include <QDir>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "fixtures.h"
#include "library.h"
#include "remote.h"
#include "sync.h"

namespace {

// The other computer, reached through an ssh that runs this build there.
QString ssh(const QTemporaryDir &dir) { return Fixtures::standInSsh(dir.path(), OMABOOK_BINARY); }

// A book put back in a collection as 0.5.2.2 did after another computer took
// it out: the row, and nothing in the log since (#300).
void putBackUnlogged(const QString &file, qint64 collectionId, qint64 bookId) {
    const QString connection = "put-back-" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(file);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QString("INSERT INTO collection_books VALUES (%1, %2, 1)").arg(collectionId).arg(bookId)));
        QVERIFY(query.exec("DELETE FROM log_dirty"));
        for (const char *key : {"books", "format"})
            QVERIFY(query.exec(
                QString("UPDATE library_state SET value = (SELECT value FROM library_state WHERE key = '%1') WHERE key = 'logged_%1'")
                    .arg(key)));
        QVERIFY(query.exec("DELETE FROM library_state WHERE key = 'memberships_relogged'"));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

} // namespace

class PutBackTests : public QObject {
    Q_OBJECT

private slots:
    // Put back in a collection here after it was taken out there: in it on
    // both after the next sync, not out again (#300).
    void aBookPutBackInACollectionStaysIn() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("back.epub"), "Put Back"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("back.epub")).id;
        const qint64 theirs = there.add(dir.filePath("back.epub")).id;
        const auto made = here.addCollection("Shelf");
        QVERIFY(made && here.putInCollection(*made, mine, true));
        const Remote remote("laptop:" + dir.filePath("There"), {ssh(dir)});
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());
        QVERIFY(there.collectionNamed("Shelf")->included.contains(theirs));

        QVERIFY(there.followRule(there.collectionNamed("Shelf")->id, theirs));
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());
        QVERIFY(!here.collectionNamed("Shelf")->included.contains(mine));

        QVERIFY(here.putInCollection(*made, mine, true));
        const SyncSummary back = syncLibrary(here, remote);
        QVERIFY2(back.problems.isEmpty(), qPrintable(back.problems.join('\n')));
        QVERIFY(here.collectionNamed("Shelf")->included.contains(mine));
        QVERIFY(there.collectionNamed("Shelf")->included.contains(theirs));
    }

    // Put back before this was fixed, so never logged: logged on the next
    // opening, and in it on both after the sync (#300).
    void aBookPutBackBeforeTheFixIsLoggedOnOpening() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("back.epub"), "Put Back Before"));
        Library there(dir.filePath("There"));
        QVERIFY(there.open());
        const qint64 theirs = there.add(dir.filePath("back.epub")).id;
        const Remote remote("laptop:" + dir.filePath("There"), {ssh(dir)});
        qint64 mine = 0;
        qint64 shelf = 0;
        {
            Library here(dir.filePath("Here"));
            QVERIFY(here.open());
            mine = here.add(dir.filePath("back.epub")).id;
            const auto made = here.addCollection("Shelf");
            QVERIFY(made && here.putInCollection(*made, mine, true));
            shelf = *made;
            QVERIFY(syncLibrary(here, remote).problems.isEmpty());
            QVERIFY(there.followRule(there.collectionNamed("Shelf")->id, theirs));
            QVERIFY(syncLibrary(here, remote).problems.isEmpty());
            QVERIFY(!here.collectionNamed("Shelf")->included.contains(mine));
        }
        putBackUnlogged(dir.filePath("Here/omabook.db"), shelf, mine);
        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        QVERIFY(here.collectionNamed("Shelf")->included.contains(mine));
        const SyncSummary back = syncLibrary(here, remote);
        QVERIFY2(back.problems.isEmpty(), qPrintable(back.problems.join('\n')));
        QVERIFY(here.collectionNamed("Shelf")->included.contains(mine));
        QVERIFY(there.collectionNamed("Shelf")->included.contains(theirs));
    }

    // Taken out of a collection there while another of its books is removed
    // there: out here too, though taking the removal touches the collection
    // here before its books are taken (#300).
    void aBookTakenOutElsewhereStaysOutThoughTheCollectionChangesInTheTake() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("out.epub"), "Taken Out"));
        QVERIFY(Fixtures::epub(dir.filePath("gone.epub"), "Gone Too"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QHash<QString, qint64> mine, theirs;
        for (const char *name : {"out", "gone"}) {
            mine.insert(name, here.add(dir.filePath(QString(name) + ".epub")).id);
            theirs.insert(name, there.add(dir.filePath(QString(name) + ".epub")).id);
        }
        const auto made = here.addCollection("Shelf");
        QVERIFY(made && here.putInCollection(*made, mine.value("out"), true) && here.putInCollection(*made, mine.value("gone"), true));
        const Remote remote("laptop:" + dir.filePath("There"), {ssh(dir)});
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());

        QVERIFY(there.followRule(there.collectionNamed("Shelf")->id, theirs.value("out")));
        QVERIFY(there.remove(theirs.value("gone")));
        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QVERIFY(!here.book(mine.value("gone")));
        QVERIFY(!here.collectionNamed("Shelf")->included.contains(mine.value("out")));
        QVERIFY(!there.collectionNamed("Shelf")->included.contains(theirs.value("out")));
        const SyncSummary settled = syncLibrary(here, remote);
        QVERIFY(!here.collectionNamed("Shelf")->included.contains(mine.value("out")));
        QVERIFY(!there.collectionNamed("Shelf")->included.contains(theirs.value("out")));
        QCOMPARE(settled.collectionsTaken + settled.collectionsSent, 0);
    }
};

QTEST_MAIN(PutBackTests)
#include "putback_tests.moc"
