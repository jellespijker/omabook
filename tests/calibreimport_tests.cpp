#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "annotationanchors.h"
#include "calibre.h"
#include "calibreimport.h"
#include "document.h"
#include "fixtures.h"
#include "formats.h"
#include "importer.h"
#include "library.h"

// What an import of a Calibre library leaves behind when it doesn't go as
// meant: cut off part way (#273), with places the book here doesn't reach
// (#276), or from a damaged metadata.db (#277). Every library here is made up,
// in a temporary folder.

namespace {

// A Calibre library and the Omabook library it is brought into.
struct World {
    QTemporaryDir dir;
    QString shelf = dir.filePath("Shelf");
    Library library{dir.filePath("Library")};
    bool ok = Fixtures::calibreShelf(shelf) && library.open();

    // Overwrites the page a table of metadata.db starts on, as damage does.
    [[nodiscard]] bool damage(const QString &table) const {
        int page = 0, size = 0;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "calibre-damage");
            database.setDatabaseName(shelf + "/metadata.db");
            if (database.open()) {
                QSqlQuery query(database);
                if (query.exec("SELECT rootpage FROM sqlite_master WHERE name = '" + table + "'") && query.next())
                    page = query.value(0).toInt();
                if (query.exec("PRAGMA page_size") && query.next())
                    size = query.value(0).toInt();
            }
            database.close();
        }
        QSqlDatabase::removeDatabase("calibre-damage");
        QFile file(shelf + "/metadata.db");
        return page > 1 && size > 0 && file.open(QIODevice::ReadWrite) && file.seek(qint64(page - 1) * size)
               && file.write(QByteArray(size, '\x5a')) == size;
    }

    [[nodiscard]] qint64 book(const QString &title) const {
        for (const Book &book : library.books())
            if (book.meta.title == title)
                return book.id;
        return 0;
    }
};

} // namespace

class CalibreImportTests : public QObject {
    Q_OBJECT

private slots:
    // As Omabook 0.5.2.1 left a library when its import was ended: the books
    // and their annotations there, nothing written down of what was taken.
    void importingAfterACutOffImportBringsNoAnnotationTwice() {
        World world;
        QVERIFY(world.ok);
        QVERIFY(importBooks(world.library, {world.shelf}).problems.isEmpty());
        const qint64 half = world.book("Half Way");
        QCOMPARE(world.library.annotations(half).size(), 3);
        // Found by their passages meanwhile, as when the book was opened.
        const auto anchored = AnnotationAnchors::anchor(world.library, half);
        QVERIFY2(anchored, qPrintable(Fixtures::whyNot(anchored)));
        QVERIFY(QFile::remove(world.library.root() + "/calibre-import.json"));

        QVERIFY(importBooks(world.library, {world.shelf}).problems.isEmpty());
        QCOMPARE(world.library.annotations(half).size(), 3);
        // And what is written down now keeps one removed here away.
        world.library.removeAnnotation(world.library.annotations(half).first().id);
        QVERIFY(importBooks(world.library, {world.shelf}).problems.isEmpty());
        QCOMPARE(world.library.annotations(half).size(), 2);
    }

    // What one book took is written down before the next, not at the end: an
    // import that never reaches its end forgets nothing.
    void whatACutOffImportTookIsRemembered() {
        World world;
        QVERIFY(world.ok);
        const Calibre::Books read = Calibre::read(world.shelf);
        const auto halfWay = std::ranges::find(read.books, QString("Half Way"), [](const Calibre::Book &book) { return book.meta.title; });
        QVERIFY(halfWay != read.books.end());
        const Library::Added added = world.library.add(halfWay->files.first(), halfWay->meta);
        QVERIFY(added.error.isEmpty());
        {
            // Never saved, as when the program is killed.
            CalibreImport::Memory memory(world.library.root());
            QVERIFY(CalibreImport::takeReading(world.library, added.id, *halfWay, memory));
        }
        // A line cut short says nothing, and costs no line after it.
        QFile journal(world.library.root() + "/calibre-import.journal");
        QVERIFY(journal.open(QIODevice::WriteOnly | QIODevice::Append));
        QVERIFY(journal.write("\n{\"99\": {\"sh") > 0);
        journal.close();
        QCOMPARE(world.library.annotations(added.id).size(), 3);
        world.library.removeAnnotation(world.library.annotations(added.id).first().id);

        QVERIFY(importBooks(world.library, {world.shelf}).problems.isEmpty());
        QCOMPARE(world.library.annotations(added.id).size(), 2);
        // An import that ends folds the journal into the file.
        QVERIFY(!journal.exists());
        QVERIFY(!CalibreImport::Memory(world.library.root()).of(added.id).isEmpty());
    }

    // Calibre keeps places of a file since replaced by one with fewer parts:
    // none goes past the book's last section (#276).
    void noPlaceGoesPastTheBooksLastSection() {
        World world;
        QVERIFY(world.ok);
        QVERIFY(Fixtures::calibreChange(
            world.shelf,
            {"UPDATE last_read_positions SET cfi = 'epubcfi(/400/2)' WHERE book = 3",
             "UPDATE annotations SET annot_data = '{\"type\": \"bookmark\", \"title\": \"far\", \"pos\": \"epubcfi(/200/2)\"}' "
             "WHERE id = 2",
             "UPDATE annotations SET annot_data = '{\"type\": \"highlight\", \"highlighted_text\": \"Half Way.\", "
             "\"spine_index\": 77}' WHERE id = 1"}));
        QVERIFY(importBooks(world.library, {world.shelf}).problems.isEmpty());
        const qint64 half = world.book("Half Way");
        const Book book = *world.library.book(half);
        const auto document = Formats::open(world.library.formatPath(half, "txt"));
        QVERIFY(document);
        const int last = document->count() - 1;
        QCOMPARE(world.library.progress(half).place.section, last);
        QCOMPARE(book.percent, 40.0);
        const QList<Annotation> notes = world.library.annotations(half);
        QCOMPARE(notes.size(), 2);
        for (const Annotation &note : notes)
            QCOMPARE(note.place.section, last);
    }

    // A table that can't be read is said, never passed over (#277).
    void aDamagedTableIsNamed_data() {
        QTest::addColumn<QString>("table");
        for (const char *table : {"authors", "annotations", "identifiers", "books_ratings_link", "custom_column_1"})
            QTest::newRow(table) << QString(table);
    }
    void aDamagedTableIsNamed() {
        QFETCH(QString, table);
        World world;
        QVERIFY(world.ok && world.damage(table));
        const ImportSummary summary = importBooks(world.library, {world.shelf});
        QCOMPARE(summary.added, 5);
        const QString problems = summary.problems.join('\n');
        const QString named = table == "books_ratings_link" ? QString("ratings") : table;
        QVERIFY2(problems.contains("can't be read (its table " + named + ")"), qPrintable(problems));
        QVERIFY(summary.failed());
    }

    // A re-import from a library that has since been damaged takes nothing away
    // that the last one brought.
    void aDamagedLibraryTakesNothingAway() {
        World world;
        QVERIFY(world.ok);
        QVERIFY(importBooks(world.library, {world.shelf}).problems.isEmpty());
        const qint64 done = world.book("Long Done"), half = world.book("Half Way");
        const Book before = *world.library.book(done);
        QCOMPARE(before.rating, 5);
        QVERIFY(world.damage("books_custom_column_4_link"));
        QVERIFY(!importBooks(world.library, {world.shelf}).problems.isEmpty());
        const Book after = *world.library.book(done);
        QCOMPARE(after.rating, before.rating);
        QCOMPARE(after.shelf, before.shelf);
        QCOMPARE(after.finished, before.finished);
        QCOMPARE(after.identifiers, before.identifiers);
        QCOMPARE(world.library.annotations(half).size(), 3);
    }
};

QTEST_MAIN(CalibreImportTests)
#include "calibreimport_tests.moc"
