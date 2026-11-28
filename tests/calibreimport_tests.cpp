#include <QDir>
#include <QFile>
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
// meant: cut off part way (#273), or with places the book here doesn't reach (#276). Every library here is made up, in a
// temporary folder.

namespace {

// A Calibre library and the Omabook library it is brought into.
struct World {
    QTemporaryDir dir;
    QString shelf = dir.filePath("Shelf");
    Library library{dir.filePath("Library")};
    bool ok = Fixtures::calibreShelf(shelf) && library.open();

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
};

QTEST_MAIN(CalibreImportTests)
#include "calibreimport_tests.moc"
