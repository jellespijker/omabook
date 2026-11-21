// Two copies of a book on one computer, one of them removed, and a copy on the
// other, as the manifest sync of 0.1.5.2 left them; both brought up to date
// and synced through the change log for the first time (#58). And two copies
// of one ISBN on one computer, one without a file, against the file alone on
// the other (#154).

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QProcess>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "changelog.h"
#include "fixtures.h"
#include "hlc.h"
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

[[nodiscard]] SyncSummary syncFrom(bool fromA, Library &a, Library &b, const QTemporaryDir &dir, bool dryRun = false) {
    const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
    const SyncOptions options{.dryRun = dryRun};
    return fromA ? syncLibrary(a, Remote("laptop:" + dir.filePath("B"), {ssh}), {}, options)
                 : syncLibrary(b, Remote("laptop:" + dir.filePath("A"), {ssh}), {}, options);
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

// A book without a file, as an import from a reading site makes one: by
// "Ann Author", with `isbn` and the names elsewhere it is given.
[[nodiscard]] qint64 entry(Library &library, const QString &title, const QString &isbn, const QList<QPair<QString, QString>> &names) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Ann Author"};
    meta.isbn = isbn;
    const qint64 id = library.addEntry(meta, false).id;
    if (id <= 0 || !library.setReading(id, "to-read", 0, {}))
        return 0;
    for (const auto &[scheme, value] : names)
        if (!library.setIdentifier(id, scheme, value))
            return 0;
    return id;
}

// The three books to read, each once and by its own name, on both sides.
void expectTitles(const Library &a, const Library &b) {
    for (const Library *library : {&a, &b}) {
        QStringList titles;
        for (const Book &book : library->books())
            titles << book.meta.title;
        titles.sort();
        QCOMPARE(titles, (QStringList{"Malkavian", "The Canterbury Tales", "The Lady of Crows"}));
    }
}

// Once synced, nothing more goes either way, from either side: details, reading or books.
void expectSettled(Library &a, Library &b, const QTemporaryDir &dir, bool fromA) {
    for (const bool side : {fromA, !fromA}) {
        const SyncSummary settled = syncFrom(side, a, b, dir, false);
        QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
        QCOMPARE(settled.titles.detailsTaken + settled.titles.detailsSent, QStringList());
        QCOMPARE(settled.received + settled.sent + settled.removedHere + settled.removedThere, 0);
        QCOMPARE(settled.readingTaken + settled.readingSent + settled.detailsUndecided, 0);
    }
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

    // A has the book twice: an entry without a file, as an import from
    // Goodreads makes one (with a picture of its own), and the book's file,
    // with the same ISBN; B has the file. Once the three are one book, each
    // side changes its details: they are decided once for it, a dry run from
    // either side lists it once and says the same of it, and one sync gives
    // both of A's copies the details and settles it. The covers compared are
    // the files' copies' (#154).
    void twoCopiesOfAnIsbnPairWithTheOneThere_data() {
        QTest::addColumn<bool>("fromA");
        QTest::newRow("synced from A, the side with two copies") << true;
        QTest::newRow("synced from B, the side with one") << false;
    }

    void twoCopiesOfAnIsbnPairWithTheOneThere() {
        QFETCH(const bool, fromA);
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "Twice Told"));
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 wished = entry(a, "Twice Told", "9780000000002", {{"goodreads", "11"}});
        const qint64 filed = a.add(dir.filePath("book.epub")).id;
        const qint64 theirs = b.add(dir.filePath("book.epub")).id;
        QVERIFY(wished > 0 && filed > 0 && theirs > 0);
        const SyncSummary paired = syncFrom(fromA, a, b, dir);
        QVERIFY2(paired.error.isEmpty() && paired.problems.isEmpty(), qPrintable(paired.error + paired.problems.join('\n')));
        QCOMPARE(paired.received + paired.sent, 0);

        // Then the entry is given a description and a picture of its own there, and B's copy, later, a publisher.
        const qint64 later = QDateTime::currentSecsSinceEpoch() + 60;
        BookMeta described = a.book(wished)->meta;
        described.description = "As Goodreads says it.";
        described.cover = QImage::fromData(Fixtures::picture(40, 60, Qt::darkRed));
        QVERIFY(a.setMeta(wished, described, nullptr, Hlc::pack(later, 0)));
        BookMeta published = b.book(theirs)->meta;
        published.publisher = "There Press";
        QVERIFY(b.setMeta(theirs, published, nullptr, Hlc::pack(later + 10, 0)));

        const SyncSummary dryA = syncFrom(true, a, b, dir, true);
        const SyncSummary dryB = syncFrom(false, a, b, dir, true);
        for (const SyncSummary *dry : {&dryA, &dryB})
            QVERIFY2(dry->error.isEmpty() && dry->problems.isEmpty(), qPrintable(dry->error + dry->problems.join('\n')));
        // B's are the newer details: A takes them, for the book once, as either side says it.
        const QStringList once{"Twice Told"};
        QCOMPARE(dryA.titles.detailsTaken, once);
        QCOMPARE(dryB.titles.detailsSent, once);
        QCOMPARE(dryA.titles.detailsSent, dryB.titles.detailsTaken);
        QCOMPARE(dryA.detailsUndecided + dryB.detailsUndecided, 0);

        const SyncSummary synced = syncFrom(fromA, a, b, dir);
        QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
        QCOMPARE(synced.detailsTaken + synced.detailsSent, 1);
        const BookMeta settledMeta = b.book(theirs)->meta;
        QCOMPARE(settledMeta.publisher, QString("There Press"));
        for (const qint64 id : {wished, filed})
            QVERIFY2(a.book(id)->meta.toJson() == settledMeta.toJson(), qPrintable(a.book(id)->meta.description));
        QCOMPARE(Library::hashFile(b.coverPath(theirs)), Library::hashFile(a.coverPath(filed)));
        expectSettled(a, b, dir, fromA);
        QCOMPARE(a.books("Twice Told").size(), 2);
        QCOMPARE(b.books("Twice Told").size(), 1);
    }

    // Books to read from Goodreads, no ISBN on two of them, and what the
    // last sync with Goodreads brought the same on all ("to-read|0|"): that
    // bookkeeping pairs none of them, so no book's details ever go to
    // another, from either side, and identical details settle (#154).
    void booksToReadFromGoodreadsNeverPairByTheirBookkeeping_data() {
        QTest::addColumn<bool>("fromA");
        QTest::addColumn<bool>("linkedBefore");
        for (const bool linkedBefore : {false, true})
            for (const bool fromA : {true, false})
                QTest::addRow("%s, synced from %s", linkedBefore ? "linked by an Omabook from before" : "never linked", fromA ? "A" : "B")
                    << fromA << linkedBefore;
    }

    void booksToReadFromGoodreadsNeverPairByTheirBookkeeping() {
        QFETCH(const bool, fromA);
        QFETCH(const bool, linkedBefore);
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const QList<QPair<QString, QString>> toRead{{"goodreads:synced", "to-read|0|"}};
        const qint64 crows = entry(a, "The Lady of Crows", {}, QList<QPair<QString, QString>>{{"goodreads", "1"}} + toRead);
        const qint64 malkavian = entry(a, "Malkavian", {}, QList<QPair<QString, QString>>{{"goodreads", "2"}} + toRead);
        const qint64 tales = entry(b, "The Canterbury Tales", "9780140424386", QList<QPair<QString, QString>>{{"goodreads", "3"}} + toRead);
        QVERIFY(crows > 0 && malkavian > 0 && tales > 0);
        const SyncSummary first = syncFrom(fromA, a, b, dir);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty(), qPrintable(first.error + first.problems.join('\n')));
        // Each side gets the books it lacks: three books, each by its own name, on both.
        QCOMPARE(first.received + first.sent, 3);
        expectTitles(a, b);
        if (linkedBefore) {
            // As an Omabook from before linked them, by that bookkeeping: the links stay in the log.
            const QString one = Spaces::bookId(a, crows);
            const QString other = Spaces::bookId(a, malkavian);
            const QString third = Spaces::bookId(b, tales);
            for (const auto &[id, value] : {std::pair{one, third}, std::pair{other, third}})
                ChangeLog::append(a, {.space = Spaces::personalSpace(a),
                                      .entity = "book",
                                      .id = std::max(id, value),
                                      .field = "same_as",
                                      .value = std::min(id, value),
                                      .hlc = std::nullopt});
        }

        // Then the books to read get descriptions, on one side and the other.
        const qint64 later = QDateTime::currentSecsSinceEpoch() + 60;
        BookMeta described = a.book(crows)->meta;
        described.description = "Crows.";
        QVERIFY(a.setMeta(crows, described, nullptr, Hlc::pack(later, 0)));
        BookMeta told = b.book(tales)->meta;
        told.description = "Pilgrims.";
        QVERIFY(b.setMeta(tales, told, nullptr, Hlc::pack(later + 10, 0)));

        const SyncSummary dryA = syncFrom(true, a, b, dir, true);
        const SyncSummary dryB = syncFrom(false, a, b, dir, true);
        QCOMPARE(dryA.titles.detailsTaken, QStringList{"The Canterbury Tales"});
        QCOMPARE(dryA.titles.detailsSent, QStringList{"The Lady of Crows"});
        QCOMPARE(dryB.titles.detailsTaken, dryA.titles.detailsSent);
        QCOMPARE(dryB.titles.detailsSent, dryA.titles.detailsTaken);

        const SyncSummary synced = syncFrom(fromA, a, b, dir);
        QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
        expectTitles(a, b);
        for (Library *library : {&a, &b}) {
            QCOMPARE(library->books("The Lady of Crows").first().meta.description, QString("Crows."));
            QCOMPARE(library->books("The Canterbury Tales").first().meta.description, QString("Pilgrims."));
            QCOMPARE(library->books("The Canterbury Tales").first().meta.isbn, QString("9780140424386"));
            QCOMPARE(library->books("Malkavian").first().meta.description, QString());
        }
        expectSettled(a, b, dir, fromA);
        expectTitles(a, b);
    }

    // `omabook set --meta` takes the details its file names and leaves the
    // rest: a file without "isbn" keeps the book's ISBN, which names it to a
    // sync; one that says "isbn": "" forgets it (#154).
    void setMetaLeavesWhatItsFileDoesNotName() {
        QTemporaryDir dir;
        const QString root = dir.filePath("A");
        qint64 id = 0;
        {
            Library library(root);
            QVERIFY(library.open());
            id = entry(library, "Twice Told", "9780000000002", {});
            QVERIFY(id > 0);
        }
        const auto set = [&](const QByteArray &json) {
            const QString file = dir.filePath("meta.json");
            if (!Fixtures::write(file, json))
                return false;
            QProcess process;
            process.start(QStringLiteral(OMABOOK_BINARY), {"set", QString::number(id), "--meta", file, "--library", root});
            return process.waitForFinished(60000) && process.exitCode() == 0;
        };
        QVERIFY(set(R"({"title": "Twice Told", "description": "Told again."})"));
        {
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.book(id)->meta.isbn, QString("9780000000002"));
            QCOMPARE(library.book(id)->meta.description, QString("Told again."));
            QCOMPARE(library.book(id)->meta.authors, QStringList{"Ann Author"});
        }
        QVERIFY(set(R"({"isbn": ""})"));
        Library library(root);
        QVERIFY(library.open());
        QCOMPARE(library.book(id)->meta.isbn, QString());
        QCOMPARE(library.book(id)->meta.title, QString("Twice Told"));
    }
};

QTEST_MAIN(DuplicatesTests)
#include "duplicates_tests.moc"
