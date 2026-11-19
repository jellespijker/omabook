// A library written outside Omabook: an older copy of it put back from its
// backups, or a book removed from its rows by another program. Opening it
// takes what changed into the change log (ADR 0001, Migration 5), and only
// what this computer had and lost travels from it as a removal (#61).

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>

#include "fixtures.h"
#include "library.h"
#include "remote.h"
#include "sync.h"

namespace {

[[nodiscard]] SyncSummary syncWithB(Library &a, const QTemporaryDir &dir) {
    return syncLibrary(a, Remote("laptop:" + dir.filePath("B"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)}));
}

// How many of a library's books are called `title` (a search would find
// "Mine There" for "Mine Here" too).
[[nodiscard]] qsizetype titled(const Library &library, const QString &title) {
    const QList<Book> books = library.books();
    return std::ranges::count_if(books, [&title](const Book &book) { return book.meta.title == title; });
}

// Runs `sql` on the library at `root` as another program would, Omabook not
// looking: no change log, only the rows and their counters.
[[nodiscard]] bool writeOutside(const QString &root, const QStringList &sql) {
    const QString connection = "outside-" + QUuid::createUuid().toString(QUuid::Id128);
    bool written = true;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(root + "/omabook.db");
        written = database.open();
        for (const QString &statement : sql) {
            if (QSqlQuery query(database); written && !query.exec(statement)) {
                qWarning().noquote() << query.lastError().text() << statement;
                written = false;
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return written;
}

// What another program writes that the log doesn't see row by row: how a
// book is set on the page. Only the library's counters say it changed, so
// opening it reconciles the whole library with the log.
const QString setOnThePage = "INSERT OR REPLACE INTO book_format (book_id, options) "
                             "SELECT id, '{\"size\":14}' FROM books WHERE title = 'Mine Here'";

// Puts the copy at `backup` back in place of the library at `root`, as one
// does by hand with a copy from its .backups folder.
[[nodiscard]] bool putBack(const QString &backup, const QString &root) {
    const QString database = root + "/omabook.db";
    for (const QString &file : {database, database + "-wal", database + "-shm"})
        if (QFile::exists(file) && !QFile::remove(file))
            return false;
    return QFile::copy(backup, database);
}

// A and B, paired by a first sync, each with a book of its own; then
// "Only There" is added on B.
struct TwoComputers {
    QTemporaryDir dir;
    qint64 onlyThereOnB = 0;
    QString onlyThereFile; // its file on B
};

void pairWithABookOfEach(TwoComputers &two) {
    for (const char *title : {"Mine Here", "Mine There", "Only There"})
        QVERIFY(Fixtures::epub(two.dir.filePath(QString(title) + ".epub"), title));
    Library a(two.dir.filePath("A"));
    Library b(two.dir.filePath("B"));
    QVERIFY(a.open() && b.open());
    QVERIFY(a.add(two.dir.filePath("Mine Here.epub")).id > 0);
    QVERIFY(b.add(two.dir.filePath("Mine There.epub")).id > 0);
    const SyncSummary paired = syncWithB(a, two.dir);
    QVERIFY2(paired.error.isEmpty() && paired.problems.isEmpty(), qPrintable(paired.error + paired.problems.join('\n')));
    QCOMPARE(a.books().size(), 2);
    two.onlyThereOnB = b.add(two.dir.filePath("Only There.epub")).id;
    QVERIFY(two.onlyThereOnB > 0);
    const QList<FormatFile> files = b.formatFiles(two.onlyThereOnB);
    QCOMPARE(files.size(), 1);
    two.onlyThereFile = files.first().path;
}

// Takes A's copy from before "Only There" came, after it came, and puts it
// back: the copy knows nothing of the book, and A's next sync is told of it
// again, by B and by what A itself wrote of it then.
void putBackACopyFromBefore(const TwoComputers &two) {
    const QString rootA = two.dir.filePath("A");
    Library a(rootA);
    QVERIFY(a.open());
    // A copy as Library::backUp makes the one an upgrade keeps.
    const QString backup = a.backupFolder() + QString("/omabook-format-%1-copy.db").arg(a.formatVersion());
    QVERIFY(QDir().mkpath(a.backupFolder()));
    QVERIFY(writeOutside(rootA, {QString("VACUUM INTO '%1'").arg(backup)}));
    const SyncSummary came = syncWithB(a, two.dir);
    QVERIFY2(came.error.isEmpty() && came.problems.isEmpty(), qPrintable(came.error + came.problems.join('\n')));
    QCOMPARE(came.received, 1);
    QCOMPARE(titled(a, "Only There"), 1);
    QVERIFY(putBack(backup, rootA));
}

} // namespace

class RestoringTests : public QObject {
    Q_OBJECT

private slots:
    // A is told of "Only There" by B, but its file doesn't come (B's copy
    // can't be read just then), so the log knows the book and A's rows
    // don't. Another program then sets a book on the page, and opening A
    // reconciles the whole library with the log: A never had the book, so
    // that writes no removal of it. B keeps the book, and the next sync
    // brings it to A. With `putBackCopy`, A had the book once, and the user
    // put back a copy of A from before it came: what A wrote of the book then
    // comes back to it from B, but its rows never had it.
    void aBookOnlyTheOtherHadIsNeverRemovedFromIt_data() {
        QTest::addColumn<bool>("putBackCopy");
        QTest::newRow("its file didn't come") << false;
        QTest::newRow("a copy from before it came put back") << true;
    }

    void aBookOnlyTheOtherHadIsNeverRemovedFromIt() {
        QFETCH(const bool, putBackCopy);
        TwoComputers two;
        pairWithABookOfEach(two);
        if (putBackCopy && !QTest::currentTestFailed())
            putBackACopyFromBefore(two);
        QVERIFY(!QTest::currentTestFailed());
        const QString rootA = two.dir.filePath("A");
        const QString hidden = two.onlyThereFile + ".away";
        {
            Library a(rootA);
            QVERIFY(a.open());
            QCOMPARE(titled(a, "Only There"), 0);
            QVERIFY(QFile::rename(two.onlyThereFile, hidden));
            const SyncSummary told = syncWithB(a, two.dir);
            QVERIFY(QFile::rename(hidden, two.onlyThereFile));
            QVERIFY2(told.error.isEmpty(), qPrintable(told.error));
            QCOMPARE(told.problems.size(), 1);
            QCOMPARE(titled(a, "Only There"), 0);
        }
        QVERIFY(writeOutside(rootA, {setOnThePage}));

        Library a(rootA);
        QVERIFY(a.open());
        const SyncSummary again = syncWithB(a, two.dir);
        QVERIFY2(again.error.isEmpty() && again.problems.isEmpty(), qPrintable(again.error + again.problems.join('\n')));
        QCOMPARE(again.removedThere, 0);
        QCOMPARE(again.removedHere, 0);
        QCOMPARE(again.received, 1);
        Library b(two.dir.filePath("B"));
        QVERIFY(b.open());
        QVERIFY(b.book(two.onlyThereOnB));
        QCOMPARE(titled(a, "Only There"), 1);
        QCOMPARE(titled(a, "Mine Here"), 1);

        const SyncSummary settled = syncWithB(a, two.dir);
        QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
        QCOMPARE(settled.received + settled.sent + settled.removedHere + settled.removedThere, 0);
        QVERIFY(b.book(two.onlyThereOnB));
        QCOMPARE(titled(a, "Only There"), 1);
    }

    // A copy of A put back from before a book came from B, opened as it is:
    // B keeps the book, and A gets it back.
    void aCopyPutBackFromBeforeABookCameGetsItBack() {
        TwoComputers two;
        pairWithABookOfEach(two);
        if (!QTest::currentTestFailed())
            putBackACopyFromBefore(two);
        QVERIFY(!QTest::currentTestFailed());
        Library a(two.dir.filePath("A"));
        QVERIFY(a.open());
        QCOMPARE(titled(a, "Only There"), 0);
        const SyncSummary again = syncWithB(a, two.dir);
        QVERIFY2(again.error.isEmpty() && again.problems.isEmpty(), qPrintable(again.error + again.problems.join('\n')));
        QCOMPARE(again.removedThere + again.removedHere, 0);
        Library b(two.dir.filePath("B"));
        QVERIFY(b.open());
        QVERIFY(b.book(two.onlyThereOnB));
        QCOMPARE(titled(a, "Only There"), 1);
        QCOMPARE(titled(a, "Mine Here"), 1);
    }

    // A book A took from B and then removed itself still goes from B:
    // removed through Omabook and the library then written by another
    // program, or taken out of A's rows by another program, which the rows
    // it touched say (log_dirty), or which nothing but the counters say
    // (as a program that empties log_dirty leaves it), so the whole library
    // is reconciled. Such a program doesn't say which files were removed
    // (Omabook keeps those in the library), so A still asks B for them once;
    // they aren't there any more.
    void aBookRemovedHereStillTravelsAsARemoval_data() {
        QTest::addColumn<bool>("throughOmabook");
        QTest::addColumn<QStringList>("outside");
        const QStringList takenOut{"DELETE FROM formats WHERE book_id IN (SELECT id FROM books WHERE title = 'Only There')",
                                   "DELETE FROM books WHERE title = 'Only There'"};
        QTest::newRow("removed through Omabook, then written outside it") << true << QStringList{setOnThePage};
        QTest::newRow("removed by another program") << false << takenOut;
        QTest::newRow("removed by another program, only the counters saying so")
            << false << (takenOut + QStringList{"DELETE FROM log_dirty"});
    }

    void aBookRemovedHereStillTravelsAsARemoval() {
        QFETCH(const bool, throughOmabook);
        QFETCH(const QStringList, outside);
        TwoComputers two;
        pairWithABookOfEach(two);
        QVERIFY(!QTest::currentTestFailed());
        const QString rootA = two.dir.filePath("A");
        {
            Library a(rootA);
            QVERIFY(a.open());
            const SyncSummary came = syncWithB(a, two.dir);
            QVERIFY2(came.error.isEmpty() && came.problems.isEmpty(), qPrintable(came.error + came.problems.join('\n')));
            const QList<Book> taken = a.books("Only There");
            QCOMPARE(taken.size(), 1);
            if (throughOmabook) {
                const auto removed = a.remove(taken.first().id);
                QVERIFY2(removed, qPrintable(Fixtures::whyNot(removed)));
            }
        }
        QVERIFY(writeOutside(rootA, outside));

        Library a(rootA);
        QVERIFY(a.open());
        QCOMPARE(titled(a, "Only There"), 0);
        const SyncSummary gone = syncWithB(a, two.dir);
        QVERIFY2(gone.error.isEmpty(), qPrintable(gone.error));
        QCOMPARE(gone.problems.size(), throughOmabook ? 0 : 1);
        QCOMPARE(gone.removedThere, 1);
        QCOMPARE(gone.received, 0);
        Library b(two.dir.filePath("B"));
        QVERIFY(b.open());
        QVERIFY(!b.book(two.onlyThereOnB));
        QCOMPARE(titled(a, "Only There"), 0);
        QCOMPARE(titled(a, "Mine Here"), 1);
    }
};

QTEST_MAIN(RestoringTests)
#include "restoring_tests.moc"
