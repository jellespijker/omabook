#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "backend.h"
#include "calibre.h"
#include "calibreplugin.h"
#include "filepicker.h"
#include "fixtures.h"
#include "importer.h"
#include "library.h"
#include "plugins.h"
#include "settings.h"

namespace {

QString script(const QString &path, const QByteArray &body) {
    Fixtures::write(path, "#!/bin/bash\n" + body);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

// A stand-in calibre-debug that lists three metadata sources amid Calibre's
// chatter, and leaves a line in `runs` each time it is started.
QString fakeCalibre(const QTemporaryDir &dir) {
    return script(dir.filePath("calibre-debug"),
        "echo run >> '" + dir.path().toUtf8() + "/runs'\n"
        "echo 'Some Calibre chatter'\n"
        "echo 'OMABOOK-JSON:[{\"name\": \"Amazon.com\", \"version\": \"1.3.19\", \"author\": \"Kovid\", \"description\": \"Amazon\", "
        "\"fields\": [\"title\", \"comments\"], \"identifies\": true, \"enabled\": true, \"configured\": true, \"installed\": false}, "
        "{\"name\": \"Google Images\", \"version\": \"1.0\", \"fields\": [], \"identifies\": false, \"enabled\": true, \"configured\": true}, "
        "{\"name\": \"Bookshop\", \"version\": \"1.2.3\", \"author\": \"A. Seller\", \"fields\": [\"title\", \"series\"], "
        "\"identifies\": true, \"enabled\": true, \"configured\": true, \"installed\": true}]'\n");
}

int runs(const QTemporaryDir &dir) {
    QFile file(dir.filePath("runs"));
    return file.open(QIODevice::ReadOnly) ? int(file.readAll().count('\n')) : 0;
}

// Everything Calibre in a temp folder: two libraries named in Calibre's
// settings, a third in the default place, installed plugins, and stand-ins
// for Calibre's programs. Nothing here looks at the real home.
struct World {
    QTemporaryDir dir;
    CalibrePlugin::Where where;
    QString first, second, standard;

    World() {
        first = dir.filePath("Shelf");
        second = dir.filePath("Elsewhere/Second");
        standard = dir.filePath("home/Calibre Library");
        ok = Fixtures::calibreShelf(first) && QDir().mkpath(second) && Fixtures::calibreLibrary(second)
             && QDir().mkpath(standard) && Fixtures::calibreLibrary(standard)
             && Fixtures::calibreConfig(dir.filePath("config"), {first, second});
        where.calibreDebug = fakeCalibre(dir);
        where.ebookConvert = script(dir.filePath("ebook-convert"), "exit 0\n");
        where.configFolder = dir.filePath("config");
        where.defaultLibrary = standard;
        where.cacheFolder = dir.filePath("cache");
    }
    bool ok = false;
};

QHash<QString, Book> byTitle(const Library &library) {
    QHash<QString, Book> books;
    for (const Book &book : library.books())
        books.insert(book.meta.title, book);
    return books;
}

class SilentPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// Sets an environment variable for as long as a test needs it.
class Environment {
public:
    Environment(const char *name, const QByteArray &value) : m_name(name), m_before(qgetenv(name)), m_was(qEnvironmentVariableIsSet(name)) {
        qputenv(name, value);
    }
    ~Environment() { m_was ? qputenv(m_name, m_before) : qunsetenv(m_name); }
    Environment(const Environment &) = delete;
    Environment &operator=(const Environment &) = delete;

private:
    const char *m_name;
    QByteArray m_before;
    bool m_was;
};

} // namespace

// The Calibre plugin: one switch over everything Calibre, what it finds on
// the computer, and what an import learns of the reading. All in temp folders.
class CalibreTests : public QObject {
    Q_OBJECT

private slots:
    void columnsPeopleAddAreRecognised() {
        // By label or by name, however they are spelled, and only with a type that fits.
        QCOMPARE(Calibre::columnRole("read", "Read", "bool"), QString("read"));
        QCOMPARE(Calibre::columnRole("mycol", "Is Read", "bool"), QString("read"));
        QCOMPARE(Calibre::columnRole("status", "Status", "text"), QString("shelf"));
        QCOMPARE(Calibre::columnRole("gr_shelf", "x", "enumeration"), QString("shelf"));
        QCOMPARE(Calibre::columnRole("read", "Read", "text"), QString("shelf"));
        QCOMPARE(Calibre::columnRole("date_read", "x", "datetime"), QString("finished"));
        QCOMPARE(Calibre::columnRole("x", "Finished", "datetime"), QString("finished"));
        QCOMPARE(Calibre::columnRole("progress", "x", "int"), QString("progress"));
        QCOMPARE(Calibre::columnRole("myrating", "x", "rating"), QString("rating"));
        // The columns the Goodreads Sync plugin offers to make, by its own names.
        QCOMPARE(Calibre::columnRole("goodreads_tags", "Goodreads Tags", "text"), QString("shelf"));
        QCOMPARE(Calibre::columnRole("goodreads_rating", "Goodreads Rating", "rating"), QString("rating"));
        QCOMPARE(Calibre::columnRole("goodreads_date_read", "Goodreads Date Read", "datetime"), QString("finished"));
        QCOMPARE(Calibre::columnRole("goodreads_reading_progress", "Goodreads Reading Progress", "int"), QString("progress"));
        QCOMPARE(Calibre::columnRole("lastread", "Last Read", "datetime"), QString("finished"));
        // "grrating" is all of Goodreads' readers' rating, not the reader's own.
        QCOMPARE(Calibre::columnRole("grrating", "GR Rating", "int"), QString());
        // A site's average is not the reader's own stars, and a date called
        // "shelf" is no shelf.
        QCOMPARE(Calibre::columnRole("rating", "x", "float"), QString());
        QCOMPARE(Calibre::columnRole("shelf", "x", "datetime"), QString());
        QCOMPARE(Calibre::columnRole("pages", "Pages", "int"), QString());

        QCOMPARE(Calibre::shelfFor("Currently Reading"), QString("reading"));
        QCOMPARE(Calibre::shelfFor("currently-reading"), QString("reading"));
        QCOMPARE(Calibre::shelfFor("to-read"), QString("to-read"));
        QCOMPARE(Calibre::shelfFor("Want to Read"), QString("to-read"));
        QCOMPARE(Calibre::shelfFor("Yes"), QString("read"));
        QCOMPARE(Calibre::shelfFor("favourites"), QString());
        // Calibre counts stars in halves.
        QCOMPARE(Calibre::starsFor(0), 0);
        QCOMPARE(Calibre::starsFor(1), 1);
        QCOMPARE(Calibre::starsFor(8), 4);
        QCOMPARE(Calibre::starsFor(9), 5);
        QCOMPARE(Calibre::starsFor(10), 5);
    }

    void calibreKnowsWhatWasRead() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::calibreShelf(dir.filePath("Shelf")));
        QCOMPARE(Calibre::bookCount(dir.filePath("Shelf")), 6);
        QCOMPARE(Calibre::bookCount(dir.path()), -1);
        const Calibre::Books read = Calibre::read(dir.filePath("Shelf"));
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QHash<QString, Calibre::Book> books;
        for (const Calibre::Book &book : read.books)
            books.insert(book.meta.title, book);

        // Read to the end on the device it was read on last; four stars.
        const Calibre::Book first = books.value("Calibre's Title");
        QCOMPARE(first.reading.shelf, QString("read"));
        QCOMPARE(first.reading.percent, 100.0);
        QCOMPARE(first.reading.section, 1);
        QCOMPARE(first.reading.readAt, qint64(1700001000));
        QCOMPARE(first.reading.rating, 4);
        QCOMPARE(first.uuid, QString("11111111-aaaa-4bbb-8ccc-000000000001"));
        QCOMPARE(first.identifiers.value("goodreads"), QString("4671"));
        QCOMPARE(first.identifiers.value("isbn"), QString("9781111111111"));

        // On the "currently-reading" shelf (and another that says nothing),
        // 40% in, with a highlight and a bookmark; the highlight taken away
        // in Calibre is left out.
        const Calibre::Reading half = books.value("Half Way").reading;
        QCOMPARE(half.shelf, QString("reading"));
        QCOMPARE(half.percent, 40.0);
        QCOMPARE(half.section, 2);
        QCOMPARE(half.notes.size(), 2);
        QCOMPARE(half.notes.at(0).kind, QString("highlight"));
        QCOMPARE(half.notes.at(0).text, QString("Half Way."));
        QCOMPARE(half.notes.at(0).note, QString("Remember this"));
        QCOMPARE(half.notes.at(1).kind, QString("bookmark"));
        QCOMPARE(half.notes.at(1).text, QString("Where I stopped"));

        // A shelf column, a tick that is off, and nothing else.
        QCOMPARE(books.value("Some Day").reading.shelf, QString("to-read"));
        QCOMPARE(books.value("Some Day").reading.rating, 0);
        // A tick, the day, and the reader's own stars in a column.
        const Calibre::Reading done = books.value("Long Done").reading;
        QCOMPARE(done.shelf, QString("read"));
        QCOMPARE(done.finished, QString("2021-06-05"));
        QCOMPARE(done.rating, 5);
        // A percentage kept by hand.
        QCOMPARE(books.value("By Hand").reading.percent, 30.0);
        QCOMPARE(books.value("By Hand").reading.shelf, QString());
        // Nothing is known of a book nobody opened.
        QCOMPARE(books.value("Old Format").reading.shelf, QString());
        QCOMPARE(books.value("Old Format").reading.readAt, qint64(0));
    }

    void anImportBringsTheReadingAlong() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::calibreShelf(dir.filePath("Shelf")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const ImportSummary summary = importBooks(library, {dir.filePath("Shelf")});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.added, 5);
        QCOMPARE(summary.calibreLibraries, QStringList{dir.filePath("Shelf")});
        // Found read: the one read to the end and the one ticked. Being
        // read: the one on that shelf and the one 30% in.
        QCOMPARE(summary.read, 2);
        QCOMPARE(summary.reading, 2);
        QCOMPARE(summary.updated, 0);

        const QHash<QString, Book> books = byTitle(library);
        const Book first = books.value("Calibre's Title");
        QCOMPARE(first.shelf, QString("read"));
        QCOMPARE(first.status(), QString("read"));
        QCOMPARE(first.rating, 4);
        QCOMPARE(first.percent, 100.0);
        QCOMPARE(library.progress(first.id).section, 1);
        // When Calibre says it was read, not when it was imported.
        QCOMPARE(first.lastRead, qint64(1700001000));
        QCOMPARE(first.identifiers.value("goodreads").toString(), QString("4671"));
        QCOMPARE(first.identifiers.value("google").toString(), QString("abc"));
        QCOMPARE(first.identifiers.value("isbn").toString(), QString("9781111111111"));
        QCOMPARE(first.identifiers.value("calibre").toString(), QString("11111111-aaaa-4bbb-8ccc-000000000001"));
        // Which is how another service finds the book.
        QCOMPARE(library.bookWithIdentifier("goodreads", "4671"), first.id);
        QCOMPARE(library.bookWithIdentifier("goodreads", "9001"), books.value("Long Done").id);

        const Book half = books.value("Half Way");
        QCOMPARE(half.shelf, QString("reading"));
        QCOMPARE(half.percent, 40.0);
        const QList<Annotation> notes = library.annotations(half.id);
        QCOMPARE(notes.size(), 2);
        QStringList kinds;
        for (const Annotation &note : notes)
            kinds << note.kind + ":" + note.text + ":" + note.note;
        kinds.sort();
        QCOMPARE(kinds, (QStringList{"bookmark:Where I stopped:", "highlight:Half Way.:Remember this"}));

        QCOMPARE(books.value("Some Day").shelf, QString("to-read"));
        QCOMPARE(books.value("Some Day").status(), QString("to-read"));
        const Book done = books.value("Long Done");
        QCOMPARE(done.shelf, QString("read"));
        QCOMPARE(done.finished, QString("2021-06-05"));
        QCOMPARE(done.rating, 5);
        QCOMPARE(done.identifiers.value("amazon").toString(), QString("B00LONGDONE"));
        // No shelf was named, so how far it was read says it is being read.
        QCOMPARE(books.value("By Hand").shelf, QString());
        QCOMPARE(books.value("By Hand").percent, 30.0);
        QCOMPARE(books.value("By Hand").status(), QString("reading"));
    }

    void importingAgainUpdatesWithoutUndoingWhatWasChangedHere() {
        QTemporaryDir dir;
        const QString shelf = dir.filePath("Shelf");
        QVERIFY(Fixtures::calibreShelf(shelf));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QCOMPARE(importBooks(library, {shelf}).added, 5);
        QHash<QString, Book> books = byTitle(library);
        const qint64 first = books.value("Calibre's Title").id, half = books.value("Half Way").id,
                     some = books.value("Some Day").id, done = books.value("Long Done").id, hand = books.value("By Hand").id;

        // Importing again at once changes nothing and adds nothing.
        const ImportSummary same = importBooks(library, {shelf});
        QCOMPARE(same.added, 0);
        QCOMPARE(same.duplicates, 5);
        QCOMPARE(same.updated, 0);
        QCOMPARE(same.read, 2);
        QCOMPARE(library.annotations(half).size(), 2);

        // Meanwhile, in Omabook: stars changed, a book taken up, a highlight
        // removed, and the first book opened again.
        QVERIFY(library.setReading(done, "read", 2, "2021-06-05"));
        QVERIFY(library.setReading(some, "reading", 0, ""));
        library.removeAnnotation(library.annotations(half).first().id);
        Progress reread;
        reread.percent = 20;
        library.setProgress(first, reread);
        QVERIFY(library.setIdentifier(first, "google", "mine"));

        // And in Calibre: everything moved on.
        QVERIFY(Fixtures::calibreChange(shelf, {
            // Half Way was finished, and left the currently-reading shelf.
            "DELETE FROM books_custom_column_2_link WHERE book = 3",
            "UPDATE last_read_positions SET pos_frac = 0.99, epoch = 1700009000.0 WHERE book = 3",
            "INSERT INTO annotations VALUES (9, 3, 'TXT', 'local', 'viewer', 1700009100.0, 'h-9', 'highlight', "
            "'{\"type\": \"highlight\", \"highlighted_text\": \"The end.\", \"spine_index\": 0}', '')",
            // Some Day moved to another shelf there too, and got stars.
            "UPDATE books_custom_column_2_link SET value = 1 WHERE book = 4",
            "INSERT INTO books_ratings_link VALUES (5, 4, 1)",
            // Long Done: other stars, another day.
            "UPDATE custom_column_4 SET value = 6",
            "UPDATE custom_column_3 SET value = '2022-01-01 00:00:00+00:00' WHERE book = 5",
            // By Hand: further along. The first book: another name elsewhere.
            "UPDATE custom_column_5 SET value = 60 WHERE book = 6",
            "INSERT INTO identifiers VALUES (20, 1, 'kobo', 'k-1')",
            "UPDATE identifiers SET val = 'xyz' WHERE book = 1 AND type = 'google'",
        }));

        const ImportSummary again = importBooks(library, {shelf});
        QVERIFY2(again.problems.isEmpty(), qPrintable(again.problems.join('\n')));
        QCOMPARE(again.added, 0);
        QCOMPARE(again.duplicates, 5);
        QCOMPARE(again.updated, 5);
        QCOMPARE(library.books().size(), 5);
        books = byTitle(library);

        // What Calibre changed and nobody touched here follows Calibre.
        QCOMPARE(books.value("Half Way").shelf, QString("read"));
        QCOMPARE(books.value("Half Way").percent, 99.0);
        QCOMPARE(books.value("By Hand").percent, 60.0);
        QCOMPARE(books.value("Calibre's Title").identifiers.value("kobo").toString(), QString("k-1"));
        QCOMPARE(books.value("Some Day").rating, 4);
        QCOMPARE(books.value("Long Done").finished, QString("2022-01-01"));
        // What was changed here stays as the user left it.
        QCOMPARE(books.value("Long Done").rating, 2);
        QCOMPARE(books.value("Some Day").shelf, QString("reading"));
        QCOMPARE(books.value("Calibre's Title").identifiers.value("google").toString(), QString("mine"));
        // The later reading wins: the book was opened here after Calibre's place.
        QCOMPARE(books.value("Calibre's Title").percent, 20.0);
        // The new highlight came; the one removed here did not come back.
        QCOMPARE(library.annotations(half).size(), 2);
        QCOMPARE(again.read, 3);

        // A third import finds nothing left to do.
        QCOMPARE(importBooks(library, {shelf}).updated, 0);
        QCOMPARE(byTitle(library).value("By Hand").percent, 60.0);
        Q_UNUSED(hand);
    }

    void aBookThatWasAlreadyHereKeepsWhatWasSaidOfIt() {
        QTemporaryDir dir;
        const QString shelf = dir.filePath("Shelf");
        QVERIFY(Fixtures::calibreShelf(shelf));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        // Added by hand before Calibre was ever imported, and shelved here.
        const Library::Added added = library.add(shelf + "/Reader, Ann/Long Done (5)/Long Done - Reader, Ann.txt");
        QVERIFY2(added.error.isEmpty(), qPrintable(added.error));
        QVERIFY(library.setReading(added.id, "reading", 3, ""));

        const ImportSummary summary = importBooks(library, {shelf});
        QCOMPARE(summary.added, 4);
        QCOMPARE(summary.duplicates, 1);
        const Book book = *library.book(added.id);
        // The user's shelf and stars stand; what was empty is filled in.
        QCOMPARE(book.shelf, QString("reading"));
        QCOMPARE(book.rating, 3);
        QCOMPARE(book.finished, QString("2021-06-05"));
        QCOMPARE(book.identifiers.value("goodreads").toString(), QString("9001"));
    }

    void aBookWhoseFileIsHereAlreadyIsAlreadyHereNotAProblem() {
        QTemporaryDir dir;
        const QString calibre = dir.filePath("Calibre Library");
        QVERIFY(Fixtures::calibreLibrary(calibre));
        const QString folder = calibre + "/Writer, Bo/Shelved Wrong (1)/";
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        // Calibre's two files of one book, here as two books.
        const Library::Added epub = library.add(folder + "Shelved Wrong - Writer, Bo.epub");
        const Library::Added txt = library.add(folder + "Shelved Wrong - Writer, Bo.txt");
        QVERIFY(epub.error.isEmpty() && txt.error.isEmpty());

        const ImportSummary summary = importBooks(library, {calibre});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.added, 0);
        QCOMPARE(summary.joined, 0);
        QCOMPARE(summary.duplicates, 1);
        QVERIFY(summary.items.value(0).duplicate);
        // Neither file was copied in a second time.
        QCOMPARE(library.books().size(), 2);
        QCOMPARE(library.book(epub.id)->formats, QStringList{"epub"});
        QCOMPARE(library.book(txt.id)->formats, QStringList{"txt"});
    }

    void aFileAnotherImportPutInPlaceIsNoProblem() {
        QTemporaryDir dir;
        const QString calibre = dir.filePath("Calibre Library");
        QVERIFY(Fixtures::calibreLibrary(calibre));
        const QString folder = calibre + "/Writer, Bo/Shelved Wrong (1)/";
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "Calibre's Title";
        meta.authors = {"Bo Writer"};
        const Library::Added added = library.add(folder + "Shelved Wrong - Writer, Bo.epub", meta);
        QVERIFY(added.error.isEmpty());
        // Another import (the window's, say) is copying the text file in at
        // the same time: it has put the file where this one would, and its
        // own copy is still under way beside it.
        const QString target = library.root() + "/" + library.book(added.id)->path + "/Calibre's Title - Bo Writer.txt";
        QVERIFY(QFile::copy(folder + "Shelved Wrong - Writer, Bo.txt", target));
        QVERIFY(QDir().mkpath(target + ".part"));

        const ImportSummary summary = importBooks(library, {calibre});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.duplicates, 1);
        QCOMPARE(library.book(added.id)->formats, (QStringList{"epub", "txt"}));
        QCOMPARE(library.formatPath(added.id, "txt"), target);
        QVERIFY(QFileInfo(target + ".part").isDir());
    }

    void aCopyThatFailsIsStillAProblem() {
        QTemporaryDir dir;
        const QString calibre = dir.filePath("Calibre Library");
        QVERIFY(Fixtures::calibreLibrary(calibre));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "Calibre's Title";
        meta.authors = {"Bo Writer"};
        const Library::Added added = library.add(calibre + "/Writer, Bo/Shelved Wrong (1)/Shelved Wrong - Writer, Bo.epub", meta);
        QVERIFY(added.error.isEmpty());
        const QString folder = library.root() + "/" + library.book(added.id)->path;
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));

        const ImportSummary summary = importBooks(library, {calibre});
        QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QCOMPARE(summary.duplicates, 1);
        QCOMPARE(summary.problems.size(), 1);
        QVERIFY2(summary.problems.first().contains("can't be copied into the library"), qPrintable(summary.problems.first()));
        QCOMPARE(library.book(added.id)->formats, QStringList{"epub"});
    }

    void aBookHereWithoutAFileGetsCalibresFile() {
        QTemporaryDir dir;
        const QString calibre = dir.filePath("Calibre Library");
        QVERIFY(Fixtures::calibreLibrary(calibre));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        // On a real shelf, known to Goodreads by Calibre's book's number, by
        // another title, with the user's own shelf and stars.
        BookMeta paper;
        paper.title = "The Paper Copy";
        paper.authors = {"B. Writer"};
        paper.publisher = "Paper House";
        const Library::Added entry = library.addEntry(paper, true);
        QVERIFY(entry.error.isEmpty());
        QVERIFY(library.setIdentifier(entry.id, "goodreads", "4671"));
        QVERIFY(library.setReading(entry.id, "to-read", 2, {}));

        const ImportSummary summary = importBooks(library, {calibre});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.added, 0);
        QCOMPARE(summary.joined, 1);
        QVERIFY(summary.items.value(0).joined);
        QCOMPARE(library.books().size(), 1);
        const Book book = *library.book(entry.id);
        QCOMPARE(book.formats, (QStringList{"epub", "txt"}));
        QVERIFY(book.paper);
        QCOMPARE(book.shelf, QString("to-read"));
        QCOMPARE(book.rating, 2);
        // Its details stay; Calibre's fill only what it lacked.
        QCOMPARE(book.meta.title, QString("The Paper Copy"));
        QCOMPARE(book.meta.authors, QStringList{"B. Writer"});
        QCOMPARE(book.meta.publisher, QString("Paper House"));
        QCOMPARE(book.meta.description, QString("Described by hand."));
        QCOMPARE(book.meta.series, QString("Curated Series"));
        QVERIFY(book.hasCover);
        QCOMPARE(book.identifiers.value("goodreads").toString(), QString("4671"));
        QCOMPARE(book.identifiers.value("calibre").toString(), QString("11111111-aaaa-4bbb-8ccc-000000000001"));
        // And a second import finds it here.
        const ImportSummary again = importBooks(library, {calibre});
        QCOMPARE(again.joined, 0);
        QCOMPARE(again.duplicates, 1);
    }

    void aWishByTitleAndAuthorGetsCalibresFileButNotANearMiss() {
        QTemporaryDir dir;
        const QString calibre = dir.filePath("Calibre Library");
        QVERIFY(Fixtures::calibreLibrary(calibre));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const auto entry = [&library](const QString &title, const QString &author, const QString &series, double index) {
            BookMeta meta;
            meta.title = title;
            meta.authors = {author};
            meta.series = series;
            meta.seriesIndex = index;
            const Library::Added added = library.addEntry(meta, false);
            library.setReading(added.id, "to-read", 0, {});
            return added.id;
        };
        // Not the same book: the next one of the series, and the same title by someone else.
        const qint64 next = entry("Calibre's Title", "Bo Writer", "Curated Series", 5);
        const qint64 other = entry("Calibre's Title", "Ann Other", {}, 0);
        const ImportSummary missed = importBooks(library, {calibre});
        QVERIFY2(missed.problems.isEmpty(), qPrintable(missed.problems.join('\n')));
        QCOMPARE(missed.joined, 0);
        QCOMPARE(missed.added, 1);
        QVERIFY(library.book(next)->formats.isEmpty());
        QVERIFY(library.book(other)->formats.isEmpty());

        // The same title written otherwise, by one of its authors spelled otherwise.
        QTemporaryDir second;
        Library wishes(second.filePath("Library"));
        QVERIFY(wishes.open());
        BookMeta meta;
        meta.title = "Calibre's Title (Curated Series #4)";
        meta.authors = {"Writer, Bo"};
        const Library::Added wish = wishes.addEntry(meta, false);
        QVERIFY(wishes.setReading(wish.id, "to-read", 0, {}));
        const ImportSummary summary = importBooks(wishes, {calibre});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.joined, 1);
        QCOMPARE(summary.added, 0);
        QCOMPARE(wishes.books().size(), 1);
        QCOMPARE(wishes.book(wish.id)->formats, (QStringList{"epub", "txt"}));
        QCOMPARE(wishes.book(wish.id)->shelf, QString("to-read"));
        QCOMPARE(wishes.book(wish.id)->meta.title, QString("Calibre's Title (Curated Series #4)"));
    }

    void theLibrariesCalibreKnowsAreFound() {
        World world;
        QVERIFY(world.ok);
        // A folder with books but no metadata.db is no Calibre library.
        QVERIFY(QDir().mkpath(world.dir.filePath("config/../Loose")));
        Settings settings(world.dir.filePath("omabook.json"));
        const CalibrePlugin calibre(&settings, world.where);
        QVERIFY(calibre.installed());
        QVERIFY(calibre.found());
        QVERIFY(calibre.enabled());
        QVERIFY(!calibre.switchedOff());

        // The one Calibre has open, the others it has used (most used
        // first), then its default folder. The library Calibre remembers but
        // that is gone is left out.
        const QList<CalibreLibrary> libraries = calibre.libraries();
        QCOMPARE(libraries.size(), 3);
        QCOMPARE(libraries.at(0).path, world.first);
        QCOMPARE(libraries.at(0).name, QString("Shelf"));
        QCOMPARE(libraries.at(0).books, 6);
        QCOMPARE(libraries.at(1).path, world.second);
        QCOMPARE(libraries.at(1).books, 2);
        QCOMPARE(libraries.at(2).path, world.standard);
        QCOMPARE(libraries.at(2).answer, QString());
        QCOMPARE(libraries.at(0).toJson().value("books").toInt(), 6);

        // With no Calibre settings at all, the default folder is still found.
        CalibrePlugin::Where bare;
        bare.defaultLibrary = world.standard;
        bare.cacheFolder = world.where.cacheFolder;
        const CalibrePlugin onlyDefault(&settings, bare);
        QVERIFY(!onlyDefault.installed());
        QVERIFY(onlyDefault.enabled());
        QCOMPARE(onlyDefault.libraries().size(), 1);
    }

    void itIsOnWhereCalibreIsFoundAndTheUsersChoiceWins() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("omabook.json"));
        CalibrePlugin::Where nothing;
        nothing.cacheFolder = dir.filePath("cache");
        CalibrePlugin none(&settings, nothing);
        // Nothing of Calibre's here: off, though nobody switched it off.
        QVERIFY(!none.found());
        QVERIFY(!none.enabled());
        QVERIFY(!none.switchedOff());
        QVERIFY(none.libraries().isEmpty());
        QVERIFY(none.converter().isEmpty());
        QVERIFY(none.converterProblem().contains("isn't installed"));
        // A Calibre library the user points at is still read as one.
        QVERIFY(Fixtures::calibreLibrary(dir.filePath("Pointed At")));
        QVERIFY(none.reads(dir.filePath("Pointed At")));
        QVERIFY(none.importOptions().calibre);
        QVERIFY(!none.reads(dir.path()));

        // Switched on by hand, it is on; switched off, off, and remembered.
        none.setEnabled(true);
        QVERIFY(none.enabled());
        none.setEnabled(false);
        QVERIFY(none.switchedOff());
        Settings reopened(dir.filePath("omabook.json"));
        const CalibrePlugin later(&reopened, nothing);
        QVERIFY(later.switchedOff());
        QVERIFY(!later.reads(dir.filePath("Pointed At")));
    }

    void switchedOffNothingOfCalibresIsRunReadOrOffered() {
        World world;
        QVERIFY(world.ok);
        Settings settings(world.dir.filePath("omabook.json"));
        CalibrePlugin calibre(&settings, world.where);
        calibre.setEnabled(false);
        QVERIFY(!calibre.enabled());

        // No libraries, no offers, no plugins, and Calibre was never started.
        QVERIFY(calibre.libraries().isEmpty());
        QVERIFY(calibre.librariesToOffer(true).isEmpty());
        QVERIFY(calibre.addOns(true).isEmpty());
        QVERIFY(calibre.knownAddOns().isEmpty());
        QCOMPARE(runs(world.dir), 0);
        QVERIFY(!QFile::exists(world.where.cacheFolder + "/calibre-plugins.json"));
        // No converter, with the reason.
        QVERIFY(calibre.converter().isEmpty());
        QVERIFY(calibre.converterProblem().contains("switched off"));
        // The registry shows the plugin, off, and says so on what it did.
        QHash<QString, PluginInfo> rows;
        for (const PluginInfo &plugin : calibre.entries())
            rows.insert(plugin.id, plugin);
        QCOMPARE(rows.size(), 3);
        QVERIFY(rows.value("calibre").switchable);
        QVERIFY(!rows.value("calibre").enabled);
        QVERIFY(rows.value("conversion:Calibre conversion").problem.contains("switched off"));
        QVERIFY(rows.value("import:Calibre Library").problem.contains("switched off"));
        PluginInfo amazon;
        amazon.id = "calibre:Amazon.com";
        amazon.name = "Amazon.com";
        amazon.origin = "calibre";
        QObject owner;
        QVERIFY(!calibre.source(amazon, &owner));

        // A Calibre library is then a folder of books like any other: the
        // book is what its file says it is, in each format apart, and
        // nothing of Calibre's reading comes along.
        QVERIFY(!calibre.reads(world.second));
        Library library(world.dir.filePath("Library"));
        QVERIFY(library.open());
        const ImportSummary plain = importBooks(library, {world.second}, {}, calibre.importOptions());
        QVERIFY(plain.calibreLibraries.isEmpty());
        QCOMPARE(plain.read, 0);
        const QHash<QString, Book> books = byTitle(library);
        QVERIFY(books.contains("The Test Book"));
        QVERIFY(!books.contains("Calibre's Title"));
        QCOMPARE(books.value("The Test Book").rating, 0);
        QCOMPARE(books.value("The Test Book").shelf, QString());
        QVERIFY(books.value("The Test Book").identifiers.isEmpty());
        QVERIFY(!QFile::exists(library.root() + "/calibre-import.json"));

        // Switched on again, all of it is back.
        calibre.setEnabled(true);
        QCOMPARE(calibre.libraries().size(), 3);
        QVERIFY(!calibre.addOns().isEmpty());
        QCOMPARE(runs(world.dir), 1);
        QCOMPARE(calibre.converter(), world.where.ebookConvert);
        QVERIFY(calibre.converterProblem().isEmpty());
        QVERIFY(calibre.source(amazon, &owner));
        QVERIFY(calibre.reads(world.second));
    }

    void theRegistrySwitchesAllOfCalibreWithOneEntry() {
        QTemporaryDir dir;
        // The registry takes Calibre's settings folder from the environment
        // when it is given a stand-in for Calibre, as it is here.
        const Environment cache("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        const Environment config("CALIBRE_CONFIG_DIRECTORY", dir.filePath("config").toUtf8());
        QVERIFY(Fixtures::calibreLibrary(dir.filePath("Shelf")));
        QVERIFY(Fixtures::calibreConfig(dir.filePath("config"), {dir.filePath("Shelf")}));
        Settings settings(dir.filePath("omabook.json"));
        Plugins plugins(&settings, dir.filePath("plugins"), fakeCalibre(dir));
        QNetworkAccessManager network;
        QObject owner;
        auto ids = [&] {
            QStringList ids;
            for (const PluginInfo &plugin : plugins.list())
                ids << plugin.id;
            return ids;
        };
        auto calibreSources = [&] {
            int count = 0;
            for (MetadataSource *source : plugins.sources(plugins.list(), &network, &owner))
                count += source->name().endsWith("Calibre") ? 1 : 0;
            return count;
        };

        // On: the Calibre plugin leads the list, with what was found
        // installed in Calibre beneath it and its sources among the sources.
        QStringList listed = ids();
        QCOMPARE(listed.first(), QString("calibre"));
        for (const char *expected : {"calibre:Amazon.com", "calibre:Bookshop", "calibre:Tidy Import", "calibre:Pocket Reader",
                                     "conversion:Calibre conversion", "import:Calibre Library"})
            QVERIFY2(listed.contains(expected), expected);
        QCOMPARE(calibreSources(), 2);
        // Each of Calibre's sources stays switchable on its own.
        QVERIFY(plugins.setEnabled("calibre:Bookshop", false));
        QCOMPARE(calibreSources(), 1);
        // What Omabook can't use is listed, not switched.
        QVERIFY(!plugins.setEnabled("calibre:Pocket Reader", false));

        // Off: one entry is left, and no source of Calibre's is asked.
        QVERIFY(plugins.setEnabled("calibre", false));
        listed = ids();
        QVERIFY(listed.contains("calibre"));
        for (const QString &id : std::as_const(listed))
            QVERIFY2(!id.startsWith("calibre:"), qPrintable(id));
        QCOMPARE(calibreSources(), 0);
        for (const PluginInfo &plugin : plugins.known())
            if (plugin.id == "calibre")
                QVERIFY(!plugin.enabled);
        QVERIFY(!plugins.calibre().enabled());

        // On again, and the single switches are as they were left.
        QVERIFY(plugins.setEnabled("calibre", true));
        QCOMPARE(calibreSources(), 1);
    }

    void calibresPluginsAreFoundOfEveryKindAndRemembered() {
        World world;
        QVERIFY(world.ok);
        Settings settings(world.dir.filePath("omabook.json"));
        const CalibrePlugin calibre(&settings, world.where);
        // Nothing is known before Calibre was asked, and asking starts it once.
        QVERIFY(calibre.knownAddOns().isEmpty());
        QCOMPARE(runs(world.dir), 0);
        QHash<QString, CalibreAddOn> found;
        for (const CalibreAddOn &addOn : calibre.addOns())
            found.insert(addOn.name, addOn);
        QCOMPARE(runs(world.dir), 1);
        QCOMPARE(found.size(), 5);

        // A source Calibre ships with.
        QCOMPARE(found.value("Amazon.com").type, QString("Metadata source"));
        QCOMPARE(found.value("Amazon.com").kind, QString("metadata"));
        QCOMPARE(found.value("Amazon.com").fields, (QStringList{"title", "description"}));
        QVERIFY(found.value("Amazon.com").usable);
        QVERIFY(!found.value("Amazon.com").installed);
        // One that only fetches covers is listed, and can't be used.
        QVERIFY(!found.value("Google Images").usable);
        QVERIFY(found.value("Google Images").note.contains("covers"));
        // One the user installed: Calibre describes it, the zip says where it lives.
        QVERIFY(found.value("Bookshop").installed);
        QVERIFY(found.value("Bookshop").usable);
        QCOMPARE(found.value("Bookshop").file, world.where.configFolder + "/plugins/Bookshop.zip");
        // Installed plugins of kinds Omabook has no use for are reported
        // with their kind and the reason, not hidden.
        const CalibreAddOn tidy = found.value("Tidy Import");
        QCOMPARE(tidy.type, QString("File type"));
        QCOMPARE(tidy.kind, QString("import"));
        QCOMPARE(tidy.version, QString("0.4"));
        QCOMPARE(tidy.author, QString("T. Idy"));
        QCOMPARE(tidy.description, QString("Tidies books as they are added"));
        QVERIFY(tidy.installed);
        QVERIFY(!tidy.usable);
        QVERIFY(!tidy.enabledInCalibre);
        QVERIFY(tidy.note.contains("File type"));
        const CalibreAddOn device = found.value("Pocket Reader");
        QCOMPARE(device.type, QString("Device interface"));
        QCOMPARE(device.kind, QString("device"));
        QCOMPARE(device.version, QString("2.0.0"));
        QVERIFY(device.enabledInCalibre);
        QVERIFY(!device.usable);
        QCOMPARE(device.toJson().value("type").toString(), QString("Device interface"));

        // The registry's rows: sources among the sources, the rest beneath
        // the Calibre plugin, and the cover fetcher nowhere.
        QHash<QString, PluginInfo> rows;
        for (const PluginInfo &plugin : calibre.entries())
            rows.insert(plugin.id, plugin);
        QCOMPARE(rows.value("calibre:Amazon.com").kind, QString("metadata"));
        QVERIFY(rows.value("calibre:Amazon.com").switchable);
        QCOMPARE(rows.value("calibre:Tidy Import").kind, QString("calibre"));
        QVERIFY(!rows.value("calibre:Tidy Import").switchable);
        QVERIFY(!rows.value("calibre:Tidy Import").enabled);
        QVERIFY(rows.value("calibre:Tidy Import").problem.contains("File type"));
        QVERIFY(rows.value("calibre:Tidy Import").description.startsWith("File type plugin for Calibre"));
        QVERIFY(!rows.contains("calibre:Google Images"));

        // Another run of Omabook knows it all at once, without starting Calibre.
        const CalibrePlugin nextRun(&settings, world.where);
        QCOMPARE(nextRun.knownAddOns().size(), 5);
        QCOMPARE(nextRun.addOns().size(), 5);
        QCOMPARE(runs(world.dir), 1);
        // Asked to look again, it does.
        QCOMPARE(nextRun.addOns(true).size(), 5);
        QCOMPARE(runs(world.dir), 2);
        // And it looks again by itself when a plugin was added to Calibre.
        QTest::qWait(30);
        QVERIFY(Fixtures::zip(world.where.configFolder + "/plugins/corner_shop.zip",
                              {{"__init__.py", "class CornerShop(StoreBase):\n    name = 'Corner Shop'\n"}}));
        const QList<CalibreAddOn> afterwards = nextRun.addOns();
        QCOMPARE(afterwards.size(), 6);
        QCOMPARE(runs(world.dir), 3);
        QVERIFY(std::ranges::any_of(std::as_const(afterwards), [](const CalibreAddOn &addOn) {
            return addOn.name == "Corner Shop" && addOn.type == "Store" && !addOn.usable;
        }));

        // Without Calibre's programs, what is installed in its folder is
        // still found, and says it can't be run.
        CalibrePlugin::Where folderOnly = world.where;
        folderOnly.calibreDebug.clear();
        folderOnly.ebookConvert.clear();
        const CalibrePlugin noPrograms(&settings, folderOnly);
        // A list made by asking Calibre says nothing of a computer without it.
        QVERIFY(noPrograms.knownAddOns().isEmpty());
        QHash<QString, CalibreAddOn> zips;
        for (const CalibreAddOn &addOn : noPrograms.addOns())
            zips.insert(addOn.name, addOn);
        QCOMPARE(runs(world.dir), 3);
        QCOMPARE(zips.value("Bookshop").type, QString("Metadata source"));
        QCOMPARE(zips.value("Bookshop").version, QString("1.2.3"));
        QCOMPARE(zips.value("Bookshop").description, QString("Downloads details from a bookshop"));
        QVERIFY(!zips.value("Bookshop").usable);
        QVERIFY(zips.value("Bookshop").note.contains("isn't installed"));
    }

    void theAnswerToAnOfferIsRemembered() {
        World world;
        QVERIFY(world.ok);
        const QString config = world.dir.filePath("omabook.json");
        {
            Settings settings(config);
            CalibrePlugin calibre(&settings, world.where);
            // Never offered: all three are.
            QCOMPARE(calibre.librariesToOffer().size(), 3);
            calibre.answer(world.first, "never");
            calibre.answer(world.second, "later");
            QCOMPARE(calibre.librariesToOffer().size(), 1);
            QCOMPARE(calibre.librariesToOffer().first().path, world.standard);
            // Going looking brings back the one put off, not the one refused.
            QCOMPARE(calibre.librariesToOffer(true).size(), 2);
            calibre.answer(world.standard, "imported");
            QVERIFY(calibre.librariesToOffer().isEmpty());
            QCOMPARE(calibre.librariesToOffer(true).size(), 1);
            QCOMPARE(calibre.libraries().at(0).answer, QString("never"));
        }
        // The answers outlast the run.
        {
            Settings settings(config);
            const CalibrePlugin calibre(&settings, world.where);
            QVERIFY(calibre.librariesToOffer().isEmpty());
            QCOMPARE(calibre.libraries().at(2).answer, QString("imported"));
        }
        // "Not now" is not "never": after a week it is offered again.
        QFile file(config);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject values = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonObject answers = values.value("calibre_libraries").toObject();
        QJsonObject later = answers.value(world.second).toObject();
        QCOMPARE(later.value("answer").toString(), QString("later"));
        later.insert("at", later.value("at").toDouble() - 8 * 24 * 3600);
        answers.insert(world.second, later);
        values.insert("calibre_libraries", answers);
        QVERIFY(Fixtures::write(config, QJsonDocument(values).toJson()));
        {
            Settings settings(config);
            CalibrePlugin calibre(&settings, world.where);
            QCOMPARE(calibre.librariesToOffer().size(), 1);
            QCOMPARE(calibre.librariesToOffer().first().path, world.second);
            // Forgetting an answer offers the library afresh.
            calibre.answer(world.first, "");
            QCOMPARE(calibre.librariesToOffer().size(), 2);
        }
    }

    void theWindowsBackendOffersImportsAndRemembers() {
        QTemporaryDir dir;
        const Environment cache("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        const Environment config("CALIBRE_CONFIG_DIRECTORY", dir.filePath("config").toUtf8());
        const QString shelf = dir.filePath("Shelf"), second = dir.filePath("Second");
        QVERIFY(Fixtures::calibreShelf(shelf));
        QVERIFY(Fixtures::calibreLibrary(second));
        QVERIFY(Fixtures::calibreConfig(dir.filePath("config"), {shelf, second}));
        Settings settings(dir.filePath("omabook.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        SilentPicker picker;
        {
            Backend backend(&settings, &picker, nullptr, {}, [](QObject *) { return QList<MetadataSource *>(); },
                            dir.filePath("plugins"), "-");
            // The library Calibre has open is offered first, by name and size.
            QTRY_COMPARE(backend.calibreOffer().value("path").toString(), shelf);
            QCOMPARE(backend.calibreOffer().value("name").toString(), QString("Shelf"));
            QCOMPARE(backend.calibreOffer().value("books").toInt(), 6);
            QCOMPARE(backend.calibreLibraries().size(), 2);
            QVERIFY(!backend.calibreSwitchedOff());

            // "Not now" moves on to the next; "Import" brings it in on the
            // import worker and says what Calibre knew of the reading.
            backend.answerCalibreOffer("later");
            QTRY_COMPARE(backend.calibreOffer().value("path").toString(), second);
            backend.findCalibreLibraries();
            QTRY_COMPARE(backend.calibreOffer().value("path").toString(), shelf);
            backend.answerCalibreOffer("import");
            QVERIFY(backend.busy());
            QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 20000);
            QCOMPARE(backend.library()->books().size(), 5);
            QVERIFY2(backend.message().contains("5 books added"), qPrintable(backend.message()));
            QVERIFY2(backend.message().contains("2 are read"), qPrintable(backend.message()));
            QVERIFY2(backend.message().contains("2 are being read"), qPrintable(backend.message()));
            QCOMPARE(settings.calibreLibraries().value(shelf).toObject().value("answer").toString(), QString("imported"));
            // The other is still on offer; refused for good, nothing is.
            QTRY_COMPARE(backend.calibreOffer().value("path").toString(), second);
            backend.answerCalibreOffer("never");
            QTRY_VERIFY(backend.calibreOffer().isEmpty());
            backend.findCalibreLibraries();
            QTRY_VERIFY(backend.message().contains("already imported or set aside"));
            QVERIFY(backend.calibreOffer().isEmpty());

            // Switched off, nothing is offered or listed, and a Calibre
            // library added by hand is a folder of books, which is said.
            backend.setPluginEnabled("calibre", false);
            QTRY_VERIFY(backend.calibreSwitchedOff());
            QVERIFY(backend.calibreLibraries().isEmpty());
            backend.findCalibreLibraries();
            QTRY_VERIFY(backend.message().contains("switched off"));
            backend.addUrls({QUrl::fromLocalFile(second)});
            QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 20000);
            QVERIFY2(backend.message().contains("plain folder"), qPrintable(backend.message()));
        }
        // The next run asks about nothing: both were answered.
        Settings reopened(dir.filePath("omabook.json"));
        reopened.setPluginEnabled("calibre", true);
        Backend next(&reopened, &picker, nullptr, {}, [](QObject *) { return QList<MetadataSource *>(); },
                     dir.filePath("plugins"), "-");
        QTRY_COMPARE(next.calibreLibraries().size(), 2);
        QVERIFY(next.calibreOffer().isEmpty());
    }
};

QTEST_MAIN(CalibreTests)
#include "calibre_tests.moc"
