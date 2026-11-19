// Two copies of a book on one computer, one of them removed, and a copy on the
// other, as the manifest sync of 0.1.5.2 left them; both brought up to date
// and synced through the change log for the first time (#58).

#include <QDir>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "fixtures.h"
#include "library.h"
#include "remote.h"
#include "replication.h"
#include "samples.h"
#include "spacereplay.h"
#include "spaces.h"
#include "sync.h"

namespace {

constexpr qint64 inStep = 1794690000;    // when both computers' details of the book last changed
constexpr qint64 removedAt = 1794700000; // when one of its copies was removed, after that

// A copy of the book, "Twice Told" by Di Double, Calibre's 42: with a file
// of `contents`, or without a file when there are none. One `removed` is
// what removing it left in the library: the hash of its file, or the entry
// keys of a copy without one.
struct Copy {
    qint64 id = 0;
    QByteArray contents;
    bool removed = false;
};

[[nodiscard]] bool run(const QSqlDatabase &database, const QString &sql, const QVariantList &values) {
    QSqlQuery query(database);
    query.prepare(sql);
    for (const QVariant &value : values)
        query.addBindValue(value);
    if (query.exec())
        return true;
    qWarning().noquote() << query.lastError().text() << sql;
    return false;
}

void writeCopy(const QSqlDatabase &database, const QTemporaryDir &dir, const QString &root, const Copy &copy) {
    const QString folder = QString("Di Double/Twice Told (%1)").arg(copy.id);
    const QString file = "Twice Told - Di Double.md";
    if (copy.removed && copy.contents.isEmpty()) {
        for (const char *key : {"entry:calibre=42", "entry:title=twicetold|didouble"})
            QVERIFY(run(database, "INSERT INTO removed (hash, removed_at) VALUES (?, ?)", {QString(key), removedAt}));
        return;
    }
    if (copy.removed) {
        const QString gone = dir.filePath(QString("removed-%1.md").arg(copy.id));
        QVERIFY(Fixtures::write(gone, copy.contents));
        QVERIFY(run(database, "INSERT INTO removed (hash, removed_at) VALUES (?, ?)", {Library::hashFile(gone), removedAt}));
        return;
    }
    QVERIFY(QDir(root).mkpath(folder));
    QVERIFY(run(database,
                "INSERT INTO books (id, title, authors, path, added, shelf, identifiers, details_at, reading_at) "
                "VALUES (?, 'Twice Told', 'Di Double', ?, ?, 'to-read', '{\"calibre\":\"42\"}', ?, ?)",
                {copy.id, folder, inStep - 1000, inStep, inStep}));
    if (copy.contents.isEmpty())
        return;
    const QString path = root + "/" + folder + "/" + file;
    QVERIFY(Fixtures::write(path, copy.contents));
    QVERIFY(run(database, "INSERT INTO formats (book_id, format, file, size, hash) VALUES (?, 'md', ?, ?, ?)",
                {copy.id, file, copy.contents.size(), Library::hashFile(path)}));
}

// The library sample of 0.1.5 (format 12) at `root`, with `copies` of the book.
void oldLibrary(const QTemporaryDir &dir, const QString &root, const QList<Copy> &copies) {
    QVERIFY(QDir().mkpath(root));
    int format = 0;
    Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), root + "/omabook.db", format);
    QCOMPARE(format, 12);
    const QString connection = "old-" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(root + "/omabook.db");
        QVERIFY(database.open());
        for (const Copy &copy : copies)
            if (!QTest::currentTestFailed())
                writeCopy(database, dir, root, copy);
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

const QByteArray firstCopy = "# Twice Told\n\nThe first copy.\n";
const QByteArray secondCopy = "# Twice Told\n\nThe second copy.\n";

// Every copy of the book in a library (every book but the sample's three),
// as its details say it: none named after the file it came in, or without
// its author, Calibre's id or what is said of its reading.
void expectWhole(const Library &library, const QString &where) {
    const QStringList sample{"The Sea Wall", "A Paper Book", "One For Later"};
    for (const Book &book : library.books()) {
        if (sample.contains(book.meta.title))
            continue;
        QVERIFY2(book.meta.title == "Twice Told" && book.meta.authorText() == "Di Double", qPrintable(where + ": " + book.meta.title));
        QCOMPARE(book.identifiers.value("calibre").toString(), QString("42"));
        QCOMPARE(book.shelf, QString("to-read"));
    }
}

[[nodiscard]] SyncSummary syncFrom(bool fromA, Library &a, Library &b, const QTemporaryDir &dir) {
    const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
    return fromA ? syncLibrary(a, Remote("laptop:" + dir.filePath("B"), {ssh}))
                 : syncLibrary(b, Remote("laptop:" + dir.filePath("A"), {ssh}));
}

[[nodiscard]] int moved(const SyncSummary &summary) {
    return summary.received + summary.sent + summary.removedHere + summary.removedThere + summary.detailsTaken + summary.detailsSent;
}

[[nodiscard]] QSet<QString> bookIds(const Library &library) {
    QSet<QString> ids;
    for (const Book &book : library.books())
        ids.insert(Spaces::bookId(library, book.id));
    return ids;
}

} // namespace

Q_DECLARE_METATYPE(Copy)

class DuplicatesTests : public QObject {
    Q_OBJECT

private slots:
    // On A, of two copies of a book the user removed one before the update:
    // the copy without a file, by its entry keys, or the one with a file, by
    // its hash. B had a copy without a file, by the same name as A's removed
    // copy. The first sync through the log leaves B its copy, because A
    // still has the book; a copy both had of the removed one goes (the
    // removal of a duplicate still travels), and the next sync has nothing
    // to do.
    void aRemovedDuplicateLeavesTheOtherComputersCopy_data() {
        QTest::addColumn<bool>("fromA");
        QTest::addColumn<QList<Copy>>("onA");
        QTest::addColumn<QList<Copy>>("onB");
        QTest::addColumn<QList<qint64>>("keptOnB");
        const QList<Copy> fileless{{.id = 10, .contents = {}, .removed = false}};
        for (const bool fromA : {true, false}) {
            const char *from = fromA ? "A" : "B";
            QTest::addRow("the copy without a file removed, synced from %s", from)
                << fromA << QList<Copy>{{.id = 10, .contents = {}, .removed = true}, {.id = 11, .contents = secondCopy, .removed = false}}
                << fileless << QList<qint64>{10};
            QTest::addRow("a copy with a file removed, synced from %s", from)
                << fromA
                << QList<Copy>{{.id = 10, .contents = firstCopy, .removed = true}, {.id = 11, .contents = secondCopy, .removed = false}}
                << fileless << QList<qint64>{10};
            QTest::addRow("both had both copies, synced from %s", from)
                << fromA
                << QList<Copy>{{.id = 10, .contents = firstCopy, .removed = true}, {.id = 11, .contents = secondCopy, .removed = false}}
                << QList<Copy>{{.id = 10, .contents = firstCopy, .removed = false}, {.id = 11, .contents = secondCopy, .removed = false}}
                << QList<qint64>{11};
        }
    }

    void aRemovedDuplicateLeavesTheOtherComputersCopy() {
        QFETCH(const bool, fromA);
        QFETCH(const QList<Copy>, onA);
        QFETCH(const QList<Copy>, onB);
        QFETCH(const QList<qint64>, keptOnB);
        QTemporaryDir dir;
        oldLibrary(dir, dir.filePath("A"), onA);
        oldLibrary(dir, dir.filePath("B"), onB);
        QVERIFY(!QTest::currentTestFailed());
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());

        const SyncSummary first = syncFrom(fromA, a, b, dir);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty(), qPrintable(first.error + first.problems.join('\n')));
        for (const Copy &copy : onB)
            QCOMPARE(bool(b.book(copy.id)), keptOnB.contains(copy.id));
        QVERIFY(a.book(11));
        QVERIFY(!a.book(10));
        expectWhole(a, "A");
        expectWhole(b, "B");

        const SyncSummary settled = syncFrom(fromA, a, b, dir);
        QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
        QCOMPARE(moved(settled), 0);
        for (const qint64 id : keptOnB)
            QVERIFY(b.book(id));
        QVERIFY(a.book(11));
    }

    // A library 0.1.6 already did it to: the first sync paired the two, linked
    // B's copy to A's kept one, and removed B's copy at the time A removed
    // its duplicate, as that sync's take did. At the next sync B gets the
    // book back from A, with its details and reading; and that removal on B,
    // which wasn't the user's, never removes A's copy.
    void aCopyLostToADuplicatesRemovalComesBack_data() {
        QTest::addColumn<bool>("fromA");
        QTest::newRow("synced from A") << true;
        QTest::newRow("synced from B") << false;
    }

    void aCopyLostToADuplicatesRemovalComesBack() {
        QFETCH(const bool, fromA);
        QTemporaryDir dir;
        oldLibrary(dir, dir.filePath("A"),
                   {{.id = 10, .contents = {}, .removed = true}, {.id = 11, .contents = secondCopy, .removed = false}});
        oldLibrary(dir, dir.filePath("B"), {{.id = 10, .contents = {}, .removed = false}});
        QVERIFY(!QTest::currentTestFailed());
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const QString spaceA = Spaces::personalSpace(a);
        Spaces::pair(b, spaceA, Spaces::deviceId(a), "A");
        Spaces::pair(a, Spaces::personalSpace(b), Spaces::deviceId(b), "B");
        QCOMPARE(Replication::link(a, Replication::unlinked(b, bookIds(a))), 1);
        SpaceReplay::takePlaces(b, Replication::since(a, Replication::vector(b)));
        SpaceReplay::takePlaces(a, Replication::since(b, Replication::vector(a)));
        QVERIFY(b.remove(10, removedAt));
        QVERIFY(b.books("Twice Told").isEmpty());

        const SyncSummary back = syncFrom(fromA, a, b, dir);
        QVERIFY2(back.error.isEmpty() && back.problems.isEmpty(), qPrintable(back.error + back.problems.join('\n')));
        QCOMPARE(fromA ? back.sent : back.received, 1);
        QCOMPARE(back.removedHere + back.removedThere, 0);
        QCOMPARE(b.books("Twice Told").size(), 1);
        QVERIFY(a.book(11));
        expectWhole(a, "A");
        expectWhole(b, "B");

        // Settled: nothing goes either way, from either side.
        for (const bool side : {fromA, !fromA}) {
            const SyncSummary settled = syncFrom(side, a, b, dir);
            QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
            QCOMPARE(moved(settled), 0);
        }
        QVERIFY(a.book(11));
        QCOMPARE(b.books("Twice Told").size(), 1);
    }
};

QTEST_MAIN(DuplicatesTests)
#include "duplicates_tests.moc"
