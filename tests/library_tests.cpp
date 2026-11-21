#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "booklistmodel.h"
#include "bookops.h"
#include "calibre.h"
#include "catalog.h"
#include "collections.h"
#include "curation.h"
#include "fixtures.h"
#include "importer.h"
#include "formats.h"
#include "graph.h"
#include "libraries.h"
#include "log.h"
#include "library.h"
#include "readingformat.h"
#include "readingsite.h"
#include "settings.h"

// The library: books copied in, described, changed, read and removed, all in
// temp folders.
class LibraryTests : public QObject {
    Q_OBJECT

private slots:
    void addingCopiesTheBookAndReadsItsDetails() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("in.epub")));
        QFile before(dir.filePath("in.epub"));
        QVERIFY(before.open(QIODevice::ReadOnly));
        const QByteArray original = before.readAll();
        before.close();

        Library library(dir.filePath("Library"));
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        const Library::Added added = library.add(dir.filePath("in.epub"));
        QVERIFY2(added.error.isEmpty(), qPrintable(added.error));
        QVERIFY(added.id > 0);
        QVERIFY(!added.duplicate);

        const auto book = library.book(added.id);
        QVERIFY(book);
        QCOMPARE(book->meta.title, QString("The Test Book"));
        QCOMPARE(book->meta.authors, (QStringList{"Ann Author", "Bo Writer"}));
        QCOMPARE(book->meta.series, QString("The Fixtures"));
        QCOMPARE(book->meta.tags, (QStringList{"Testing", "Fixtures"}));
        QCOMPARE(book->formats, QStringList{"epub"});
        QVERIFY(book->hasCover);

        // A folder a person can read, holding the copy and the cover.
        QCOMPARE(book->path, QString("Ann Author/The Test Book (%1)").arg(added.id));
        QCOMPARE(library.formatPath(added.id), dir.filePath("Library/" + book->path + "/The Test Book - Ann Author.epub"));
        QVERIFY(QFile::exists(library.formatPath(added.id)));
        QCOMPARE(QImage(library.coverPath(added.id)).size(), QSize(60, 90));

        // The file it came from is exactly as it was.
        QFile source(dir.filePath("in.epub"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(source.readAll(), original);
    }

    void theSameFileIsNotAddedTwice() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        QVERIFY(QFile::copy(dir.filePath("a.epub"), dir.filePath("renamed copy.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 first = library.add(dir.filePath("a.epub")).id;
        const Library::Added again = library.add(dir.filePath("renamed copy.epub"));
        QVERIFY(again.duplicate);
        QCOMPARE(again.id, first);
        QCOMPARE(library.books().size(), 1);
    }

    void whatIsNotABookIsRefusedWithAReason() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("photo.jpg"), "x"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(library.add(dir.filePath("photo.jpg")).error.contains("not a book format"));
        QVERIFY(library.add(dir.filePath("missing.epub")).error.contains("no readable file"));
        QVERIFY(library.books().isEmpty());
    }

    void aBookWithNoDetailsStillGoesIn() {
        // Plain text names nothing but itself: empty fields are empty, not null.
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("Loose Notes.txt"), "Some words.\n"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const Library::Added added = library.add(dir.filePath("Loose Notes.txt"));
        QVERIFY2(added.error.isEmpty(), qPrintable(added.error));
        QCOMPARE(library.book(added.id)->meta.title, QString("Loose Notes"));
        QCOMPARE(library.book(added.id)->path, QString("Unknown/Loose Notes (%1)").arg(added.id));
    }

    void anotherFormatJoinsTheSameBook() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        QVERIFY(Fixtures::write(dir.filePath("a.azw3"), Fixtures::mobi()));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("a.azw3")).id;
        QCOMPARE(library.book(id)->meta.title, QString("Kindle Fixture"));
        QString error;
        QVERIFY2(library.addFormat(id, dir.filePath("a.epub"), &error), qPrintable(error));
        // The format Omabook reads itself is preferred.
        QCOMPARE(library.book(id)->formats, (QStringList{"epub", "azw3"}));
        QVERIFY(library.formatPath(id).endsWith(".epub"));
        QVERIFY(library.formatPath(id, "azw3").endsWith(".azw3"));
        QVERIFY(library.formatPath(id, "pdf").isEmpty());
        QVERIFY(!library.addFormat(99, dir.filePath("a.epub"), &error));
    }

    void changingDetailsRenamesTheFolder() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("a.epub")).id;

        BookMeta meta = library.book(id)->meta;
        meta.title = "A New: Title?";
        meta.authors = QStringList{"Cy Changed"};
        meta.series.clear();
        meta.tags = QStringList{"one", "two"};
        meta.cover = QImage(100, 1800, QImage::Format_RGB32);
        meta.cover.fill(Qt::blue);
        QVERIFY(library.setMeta(id, meta));

        const auto book = library.book(id);
        QCOMPARE(book->meta.title, QString("A New: Title?"));
        QCOMPARE(book->meta.authors, QStringList{"Cy Changed"});
        QCOMPARE(book->meta.series, QString());
        QCOMPARE(book->meta.tags, (QStringList{"one", "two"}));
        // Characters a filesystem may refuse are left out of the folder's name.
        QCOMPARE(book->path, QString("Cy Changed/A New Title (%1)").arg(id));
        QVERIFY(QFile::exists(library.formatPath(id)));
        QVERIFY(!QDir(dir.filePath("Library/Ann Author")).exists());
        // A large cover is scaled down to what a shelf needs.
        QCOMPARE(QImage(library.coverPath(id)).height(), 900);
    }

    void removingTakesTheFolderAway() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("a.epub")).id;
        const QString folder = QFileInfo(library.formatPath(id)).absolutePath();
        library.setProgress(id, {1, 0.5, 75});
        QVERIFY(library.remove(id));
        QVERIFY(!QDir(folder).exists());
        QVERIFY(!library.book(id));
        QCOMPARE(library.progress(id).place.section, 0);
        QVERIFY(QFile::exists(dir.filePath("a.epub")));
        const auto again = library.remove(id);
        QVERIFY(!again && !again.error().isEmpty());
    }

    void aCalibreLibraryComesWithCalibresOwnDetails() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::calibreLibrary(dir.filePath("Calibre Library")));
        QVERIFY(Calibre::isLibrary(dir.filePath("Calibre Library")));
        QVERIFY(!Calibre::isLibrary(dir.path()));

        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QList<int> steps;
        const ImportSummary summary = importBooks(library, {dir.filePath("Calibre Library")},
                                                  [&](int done, int total) { steps << done << total; });
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        // The book in a format Omabook doesn't take is left out.
        QCOMPARE(summary.added, 1);
        QCOMPARE(steps, (QList<int>{1, 1}));

        const auto book = library.book(summary.lastId);
        QVERIFY(book);
        // What Calibre says, not what the EPUB says about itself.
        QCOMPARE(book->meta.title, QString("Calibre's Title"));
        QCOMPARE(book->meta.authors, (QStringList{"Bo Writer", "Smith, Jr."}));
        QCOMPARE(book->meta.series, QString("Curated Series"));
        QCOMPARE(book->meta.seriesIndex, 4.0);
        QCOMPARE(book->meta.tags, (QStringList{"curated", "calibre"}));
        QCOMPARE(book->meta.publisher, QString("Calibre House"));
        QCOMPARE(book->meta.language, QString("nld"));
        QCOMPARE(book->meta.isbn, QString("9781111111111"));
        QCOMPARE(book->meta.description, QString("Described by hand."));
        QCOMPARE(book->meta.published, QString("1999-03-02"));
        // Every format, and Calibre's cover.
        QCOMPARE(book->formats, (QStringList{"epub", "txt"}));
        QCOMPARE(QImage(library.coverPath(book->id)).size(), QSize(80, 120));

        // Calibre's library is exactly as it was, and importing it again adds nothing.
        QVERIFY(QFile::exists(dir.filePath("Calibre Library/Writer, Bo/Shelved Wrong (1)/Shelved Wrong - Writer, Bo.epub")));
        const ImportSummary again = importBooks(library, {dir.filePath("Calibre Library")});
        QCOMPARE(again.added, 0);
        QCOMPARE(again.duplicates, 1);
        QCOMPARE(library.books().size(), 1);
    }

    void aFileJoinsTheBookHereWithoutOne() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        // On paper, rated, with an ISBN; and a wish by another author.
        BookMeta paper;
        paper.title = "The Sea Story";
        paper.authors = {"Sailor, Ann"};
        paper.isbn = "978-0-00-000000-2";
        const Library::Added onPaper = library.addEntry(paper, true);
        QVERIFY(library.setReading(onPaper.id, "read", 4, "2020-01-02"));
        BookMeta wish;
        wish.title = "Sea Story";
        wish.authors = {"Bob Other"};
        const Library::Added wished = library.addEntry(wish, false);
        QVERIFY(library.setReading(wished.id, "to-read", 0, {}));

        // A folder of books, as dropped on the window or in the file manager.
        QVERIFY(QDir().mkpath(dir.filePath("Drop")));
        QVERIFY(Fixtures::write(dir.filePath("Drop/Sea Story - Ann Sailor.txt"), "CHAPTER 1\n\nA dark night.\n"));
        QVERIFY(Fixtures::write(dir.filePath("Drop/Land Story - Ann Sailor.txt"), "CHAPTER 1\n\nA dry day.\n"));
        const ImportSummary summary = importBooks(library, {dir.filePath("Drop")});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.joined, 1);
        QCOMPARE(summary.added, 1);
        QCOMPARE(library.books().size(), 3);
        const Book joined = *library.book(onPaper.id);
        QCOMPARE(joined.formats, QStringList{"txt"});
        QVERIFY(joined.paper);
        QCOMPARE(joined.shelf, QString("read"));
        QCOMPARE(joined.rating, 4);
        QCOMPARE(joined.finished, QString("2020-01-02"));
        QCOMPARE(joined.meta.title, QString("The Sea Story"));
        QCOMPARE(joined.meta.isbn, QString("978-0-00-000000-2"));
        // The same title by someone else is another book.
        QVERIFY(library.book(wished.id)->formats.isEmpty());

        // Dropped again, the file is here already.
        const ImportSummary again = importBooks(library, {dir.filePath("Drop/Sea Story - Ann Sailor.txt")});
        QCOMPARE(again.duplicates, 1);
        QCOMPARE(again.joined, 0);
        QCOMPARE(again.items.value(0).id, onPaper.id);
    }

    void aLibraryForTheSessionLeavesTheSettingsAlone() {
        QTemporaryDir dir;
        const QString file = dir.filePath("config.json");
        QVERIFY(Fixtures::write(file, "{\"library\": \"" + dir.filePath("Books").toUtf8() + "\"}"));
        Settings settings(file);
        settings.useLibraryForSession(dir.filePath("Other"));
        QCOMPARE(settings.libraryPath(), dir.filePath("Other"));
        QVERIFY(settings.libraries().contains(dir.filePath("Other")));
        // Something else saved meanwhile keeps the library in use.
        settings.setGraph("off");
        QCOMPARE(Settings(file).libraryPath(), dir.filePath("Books"));
        QVERIFY(!Settings(file).libraries().contains(dir.filePath("Other")));
        // Switching on purpose makes it the one in use.
        settings.setLibraryPath(dir.filePath("Other"));
        QCOMPARE(Settings(file).libraryPath(), dir.filePath("Other"));
        settings.useLibraryForSession(dir.filePath("Third"));
        settings.setLibraryPath(dir.filePath("Other"));
        QCOMPARE(settings.libraryPath(), dir.filePath("Other"));
    }

    void aFolderIsSearchedForBooks() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("in/deep/er"));
        QVERIFY(Fixtures::epub(dir.filePath("in/one.epub")));
        QVERIFY(Fixtures::write(dir.filePath("in/deep/er/Two.txt"), "Words.\n"));
        QVERIFY(Fixtures::write(dir.filePath("in/deep/notes.jpg"), "x"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const ImportSummary summary = importBooks(library, {dir.filePath("in"), dir.filePath("missing.epub")});
        QCOMPARE(summary.added, 2);
        QCOMPARE(summary.problems.size(), 1);
        QCOMPARE(summary.items.size(), 3);
    }

    void aFolderThatIsOneBookGoesInAsOne() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        // A shelf folder holding an EPUB and a folder of Markdown chapters.
        QVERIFY(QDir().mkpath(dir.filePath("shelf/My Notes/img")));
        QVERIFY(Fixtures::epub(dir.filePath("shelf/one.epub")));
        QVERIFY(Fixtures::write(dir.filePath("shelf/My Notes/01.md"), "# Start\n\nWords.\n"));
        QVERIFY(Fixtures::write(dir.filePath("shelf/My Notes/02.md"), "# Onward\n\nMore words.\n"));
        QVERIFY(Fixtures::write(dir.filePath("shelf/My Notes/img/a.png"), Fixtures::picture(8, 8, Qt::blue)));
        const ImportSummary summary = importBooks(library, {dir.filePath("shelf")});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.added, 2);
        const QList<Book> notes = library.books("title:Start");
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().formats, QStringList{"mdz"});
        // It reads from the library as the folder did, and the folder is untouched.
        const auto book = Formats::open(library.formatPath(notes.first().id));
        QVERIFY(book);
        QCOMPARE(book->count(), 2);
        QVERIFY(QFileInfo::exists(dir.filePath("shelf/My Notes/02.md")));
        // The same folder again is the same book, not another.
        QCOMPARE(importBooks(library, {dir.filePath("shelf/My Notes")}).duplicates, 1);
        // A zip of Markdown is one book too, not a book per file.
        QVERIFY(Fixtures::zip(dir.filePath("guide.zip"), {{"a.md", "# Guide\n\nOne.\n"}, {"b.md", "# Two\n\nTwo.\n"}}));
        QCOMPARE(importBooks(library, {dir.filePath("guide.zip")}).added, 1);
        QCOMPARE(library.books("title:Guide").first().formats, QStringList{"mdz"});
    }

    void booksPackedInAnArchiveAreUnpacked() {
        QTemporaryDir dir;
        Fixtures::Entries packed;
        QByteArray inner;
        {
            QVERIFY(Fixtures::epub(dir.filePath("inner.epub"), "Packed Away"));
            QFile file(dir.filePath("inner.epub"));
            QVERIFY(file.open(QIODevice::ReadOnly));
            inner = file.readAll();
        }
        packed.append(qMakePair(QString("some folder/Packed Away.epub"), inner));
        packed.append(qMakePair(QString("../../escape.txt"), QByteArray("Sneaky words.\n")));
        packed.append(qMakePair(QString("readme.nfo"), QByteArray("x")));
        QVERIFY(Fixtures::zip(dir.filePath("bundle.zip"), packed));
        QVERIFY(Fixtures::zip(dir.filePath("empty.zip"), {{"notes.nfo", "x"}}));

        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const ImportSummary summary = importBooks(library, {dir.filePath("bundle.zip"), dir.filePath("empty.zip")});
        QCOMPARE(summary.added, 2);
        QCOMPARE(library.books("packed").size(), 1);
        // A name that tries to climb out of the folder is only a name.
        QCOMPARE(library.books("escape").size(), 1);
        QVERIFY(!QFile::exists(dir.filePath("escape.txt")));
        QCOMPARE(summary.problems, QStringList{"There are no books inside empty.zip."});
    }

    void theLibraryIsWrittenOutAsACatalog() {
        Book first;
        first.id = 1;
        first.meta.title = "Dune, \"Deluxe\"";
        first.meta.authors = QStringList{"Frank Herbert", "Second Author"};
        first.meta.series = "Dune";
        first.meta.seriesIndex = 1;
        first.meta.publisher = "Chilton {Books}";
        first.meta.published = "1965-08-01";
        first.meta.isbn = "9780441172719";
        first.meta.tags = QStringList{"science fiction", "classic"};
        first.formats = QStringList{"epub", "pdf"};
        first.percent = 41.6;
        Book second;
        second.id = 2;
        second.meta.title = "Untitled";

        QCOMPARE(Catalog::formats(), (QStringList{"csv", "bibtex"}));
        const QString csv = QString::fromUtf8(Catalog::make({first, second}, "csv"));
        const QStringList lines = csv.split("\r\n");
        QCOMPARE(lines.at(0), QString("id,title,authors,series,series_index,publisher,published,language,isbn,tags,formats,percent_read"));
        // Commas and quotes inside a field are quoted, as spreadsheets expect.
        QCOMPARE(lines.at(1), QString("1,\"Dune, \"\"Deluxe\"\"\",Frank Herbert & Second Author,Dune,1,Chilton {Books},1965-08-01,,"
                                      "9780441172719,\"science fiction, classic\",epub pdf,42"));
        QCOMPARE(lines.at(2), QString("2,Untitled,,,,,,,,,,0"));

        const QString bibtex = QString::fromUtf8(Catalog::make({first, second}, "bibtex"));
        QVERIFY2(bibtex.contains("@book{herbert1965dune,"), qPrintable(bibtex));
        QVERIFY(bibtex.contains("  author = {Frank Herbert and Second Author}"));
        QVERIFY(bibtex.contains("  publisher = {Chilton \\{Books\\}}"));
        QVERIFY(bibtex.contains("  year = {1965}"));
        QVERIFY(bibtex.contains("@book{untitled,"));
        QVERIFY(Catalog::make({first}, "docx").isNull());
    }

    void theLibraryIsLookedOverForThingsToTidy() {
        QCOMPARE(Curation::tidyAuthor("Weis, Margaret"), QString("Margaret Weis"));
        QCOMPARE(Curation::tidyAuthor("  margaret   weis "), QString("margaret weis"));
        QCOMPARE(Curation::tidyAuthor("V.E. Schwab"), QString("V. E. Schwab"));
        // A suffix after a comma is part of the name, not a name turned round.
        QCOMPARE(Curation::tidyAuthor("Martin Luther King, Jr."), QString("Martin Luther King, Jr."));

        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        auto add = [&](const QString &title, const QString &author, const QString &extension, const QByteArray &words) {
            const QString file = dir.filePath(QString::number(qHash(title + author + extension)) + "." + extension);
            Fixtures::write(file, words);
            BookMeta meta;
            meta.title = title;
            meta.authors = QStringList{author};
            return library.add(file, meta).id;
        };
        const qint64 first = add("Dragon Wing", "Margaret Weis", "txt", "One copy.\n");
        const qint64 second = add("dragon wing!", "Weis, Margaret", "md", "# Another copy\n");
        const qint64 third = add("Elven Star", "margaret weis", "txt", "A different book.\n");
        const qint64 other = add("Dune", "Frank Herbert", "txt", "Someone else.\n");
        library.setProgress(second, {0, 0.5, 40, 0});

        // The same title by the same author, however each is written, is one
        // book twice; the copy being read is the one that is kept.
        const QList<Proposal> duplicates = Curation::duplicates(library.books());
        QCOMPARE(duplicates.size(), 1);
        QCOMPARE(duplicates.first().bookId, second);
        QCOMPARE(duplicates.first().otherId, first);

        // One author, three spellings: the commonest proper one wins.
        const QList<Proposal> spellings = Curation::authorSpellings(library.books());
        QCOMPARE(spellings.size(), 2);
        for (const Proposal &proposal : spellings) {
            QVERIFY(proposal.bookId == second || proposal.bookId == third);
            QCOMPARE(proposal.meta.authors, QStringList{"Margaret Weis"});
        }

        // The model's reading of the titles, kept to books that are there
        // and to real changes.
        const AiRequest request = Curation::seriesRequest(library.books());
        QVERIFY(request.messages.first().text.contains("Elven Star"));
        const QList<Proposal> series = Curation::seriesFrom(
            "Here:\n[{\"id\": " + QString::number(third) + ", \"series\": \"The Death Gate Cycle\", \"series_index\": 2}, "
            "{\"id\": 999, \"series\": \"Nowhere\", \"series_index\": 1}, {\"id\": " + QString::number(other) + ", \"series\": \"\"}]",
            library.books());
        QCOMPARE(series.size(), 1);
        QCOMPARE(series.first().meta.series, QString("The Death Gate Cycle"));
        QCOMPARE(series.first().meta.seriesIndex, 2.0);
        QVERIFY(Curation::seriesFrom("I am not sure.", library.books()).isEmpty());

        // Applied: the spellings agree, the series is set, and the two copies
        // are one book with both formats and the place that was read.
        for (const Proposal &proposal : spellings + series + duplicates)
            QVERIFY(Curation::apply(library, proposal));
        QCOMPARE(library.books().size(), 3);
        QCOMPARE(library.book(second)->formats, (QStringList{"md", "txt"}));
        QCOMPARE(library.book(second)->meta.authors, QStringList{"Margaret Weis"});
        QCOMPARE(library.book(second)->percent, 40.0);
        QCOMPARE(library.book(third)->meta.series, QString("The Death Gate Cycle"));
        QVERIFY(!library.book(first));
        QVERIFY(Curation::duplicates(library.books()).isEmpty());
        QVERIFY(Curation::authorSpellings(library.books()).isEmpty());
    }

    void theSameBookIsFoundHoweverItsTitleIsWritten() {
        // One normaliser for titles and names: the library's own and a
        // reading site's.
        QCOMPARE(Curation::titleKey("5- Shadows of Self"), Curation::titleKey("Shadows of Self"));
        for (const char *title : {"06 - Oathbringer", "#3 Oathbringer", "Book 3: Oathbringer", "Oathbringer (The Stormlight Archive #3)",
                                  "Oathbringer [Kindle Edition] (Stormlight #3)", "The Oathbringer", "Oathbringer, The", "OATHBRINGER!"})
            QVERIFY2(Curation::titleKey(title) == Curation::titleKey("Oathbringer"), title);
        QCOMPARE(Curation::titleKey("6-The Bands of Mourning"), QString("bandsofmourning"));
        // A number that is the title, or part of it, stays.
        QCOMPARE(Curation::titleKey("2001: A Space Odyssey"), QString("2001aspaceodyssey"));
        QCOMPARE(Curation::titleKey("1984"), QString("1984"));
        QVERIFY(Curation::titleKey("Mistborn 2") != Curation::titleKey("Mistborn"));
        QVERIFY(!Curation::titleKey("The").isEmpty());
        QCOMPARE(ReadingSites::titleKey("5- Shadows of Self"), Curation::titleKey("Shadows of Self"));
        QCOMPARE(Curation::authorKey("Sanderson, Brandon"), Curation::authorKey("brandon  sanderson"));
        QCOMPARE(ReadingSites::authorKey("Sanderson, Brandon"), Curation::authorKey("Brandon Sanderson"));
        QCOMPARE(Curation::authorKey("V.E. Schwab"), Curation::authorKey("V. E. Schwab"));

        qint64 next = 0;
        auto book = [&next](const QString &title, const QString &author, const QString &series = {}, double index = 0) {
            Book made;
            made.id = ++next;
            made.meta.title = title;
            made.meta.authors = BookMeta::splitAuthors(author);
            made.meta.series = series;
            made.meta.seriesIndex = index;
            made.formats = QStringList{"epub"};
            return made;
        };
        // What is proposed: each pair once, smaller id first, and whether unsure.
        auto pairs = [](const QList<Proposal> &proposals) {
            QMap<QPair<qint64, qint64>, bool> found;
            for (const Proposal &proposal : proposals)
                found.insert({qMin(proposal.bookId, proposal.otherId), qMax(proposal.bookId, proposal.otherId)}, proposal.unsure);
            return found;
        };

        const QList<Book> shelf{
            book("C++ Best Practices", "Jason Turner"),                          // 1
            book("C++ Best Practices", "Jason Turner"),                          // 2: the same, as another file
            book("5- Shadows of Self", "Brandon Sanderson", "Mistborn", 5),      // 3
            book("Shadows of Self", "Brandon Sanderson"),                        // 4
            book("6-The Bands of Mourning", "Brandon Sanderson", "Mistborn", 6), // 5
            book("The Bands of Mourning: A Mistborn Novel", "Brandon Sanderson", "Mistborn Trilogy", 6), // 6
            book("Words of Radiance (Stormlight Archive, The)", "Sanderson, Brandon"),                  // 7
            book("Words of Radiance", "Brandon Sanderson", "The Stormlight Archive", 2),                // 8
            book("Elantris", "B. Sanderson"),                                    // 9
            book("Elantris", "Brandon Sanderson & Someone Else"),                // 10
            book("The Final Empire (Mistborn Trilogy, #1)", "Brandon Sanderson"), // 11
            book("The Final Empire", "Brandon Sanderson"),                       // 12
            // Not the same book as any other.
            book("Mistborn", "Brandon Sanderson"),                               // 13
            book("Mistborn 2", "Brandon Sanderson"),                             // 14
            book("The Well of Ascension", "Brandon Sanderson", "Mistborn", 2),   // 15
            book("Mistborn: The Hero of Ages", "Brandon Sanderson", "Mistborn", 3), // 16: what is left is a series' name
            book("Legion", "Brandon Sanderson"),                                 // 17
            book("Legion: Skin Deep", "Brandon Sanderson"),                      // 18: a title of its own after the colon
            book("Dune", "Frank Herbert"),                                       // 19
            book("Dune", "Someone Else"),                                        // 20: same title, another author
            book("Warbreaker", "Brandon Sanderson"),                             // 21
            book("Warbreaker (Books 1-3)", "Brandon Sanderson"),                 // 22: an omnibus
            book("Steelheart (Book 2)", "Brandon Sanderson"),                    // 23
            book("Steelheart (Book 3)", "Brandon Sanderson"),                    // 24: another number in the series
            book("Calamity", "Brandon Sanderson", "Reckoners", 3),               // 25
            book("Calamity", "Brandon Sanderson", "Reckoners", 4),               // 26
        };
        const QList<Proposal> proposals = Curation::duplicates(shelf);
        const auto found = pairs(proposals);
        const QMap<QPair<qint64, qint64>, bool> wanted{
            {{1, 2}, false}, {{3, 4}, false}, {{5, 6}, true}, {{7, 8}, false}, {{9, 10}, false}, {{11, 12}, false},
        };
        QCOMPARE(found, wanted);
        // The unsure one says why, and the others found only once their
        // titles were set apart from how they are written say so too.
        for (const Proposal &proposal : proposals) {
            if (proposal.unsure)
                QVERIFY2(proposal.why.contains("A Mistborn Novel"), qPrintable(proposal.why));
            if (QSet<qint64>{proposal.bookId, proposal.otherId} == QSet<qint64>{3, 4})
                QVERIFY(proposal.why.contains("series number"));
            if (QSet<qint64>{proposal.bookId, proposal.otherId} == QSet<qint64>{1, 2})
                QVERIFY(proposal.why.isEmpty());
        }
        // The unsure one is shown as such.
        Book kept = shelf.at(4);
        const auto unsure = std::ranges::find_if(proposals, &Proposal::unsure);
        QVERIFY(unsure != proposals.end());
        QCOMPARE(BookOps::proposalJson(kept, *unsure).value("confidence").toString(), QString("low"));

        // "Mistborn" is a series' name here, so nothing is left off down to it.
        QVERIFY(Curation::duplicates({book("Mistborn: The Well of Ascension", "Brandon Sanderson", "Mistborn", 2),
                                      book("Mistborn", "Brandon Sanderson")})
                    .isEmpty());
        // Two subtitles after one name: it is a series, and the subtitles the books.
        QVERIFY(Curation::duplicates({book("Cosmere: A Novel of Scadrial", "Brandon Sanderson"),
                                      book("Cosmere: A Novel of Roshar", "Brandon Sanderson"), book("Cosmere", "Brandon Sanderson")})
                    .isEmpty());
    }

    void aMergeKeepsWhatEitherBookKnew() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("serpent.epub"), "Serpent Mage"));
        BookMeta meta;
        meta.title = "Serpent Mage";
        meta.authors = QStringList{"Margaret Weis", "Tracy Hickman"};
        const qint64 epub = library.add(dir.filePath("serpent.epub"), meta).id;
        // The same book as Goodreads knows it, with no file.
        const qint64 entry = library.addEntry(meta, true).id;
        QVERIFY(epub > 0 && entry > 0);
        library.setIdentifier(epub, "calibre", "83355555");
        library.setIdentifier(epub, "google", "mine");
        library.setIdentifier(entry, "goodreads", "28507");
        library.setIdentifier(entry, "goodreads:synced", "read|0|");
        library.setIdentifier(entry, "google", "theirs");
        library.setIdentifier(entry, "isbn", "9780553562811");
        library.setIdentifier(entry, "generated", "some-plugin");
        QVERIFY(library.setReading(epub, "to-read", 0, {}));
        QVERIFY(library.setReading(entry, "read", 4, "2024-03-01"));
        library.setProgress(epub, {1, 0.1, 10, 0});
        library.setProgress(entry, {3, 0.5, 60, 0});
        const Annotation mark{0, "bookmark", 2, 0.25, "A line", "", 0};
        library.addAnnotation(epub, mark);
        library.addAnnotation(entry, mark); // the same bookmark twice
        library.addAnnotation(entry, {0, "highlight", 3, 0.5, "Another line", "mine", 0});
        library.setNotes(epub, "recap", {1, 0.1, "Early notes.", 0});
        library.setNotes(epub, "cast", {1, 0.1, "Who is who.", 0});
        library.setNotes(entry, "recap", {3, 0.5, "Later notes.", 0});
        Graph graph(library);
        const qint64 epubNode = graph.bookNode(epub), entryNode = graph.bookNode(entry);
        QVERIFY(epubNode > 0 && entryNode > 0);
        const qint64 haplo = graph.upsert({}, "character", "Haplo", "A Patryn.", {}, GraphPlace{entry, 3, 0.4});
        QVERIFY(graph.link(haplo, entryNode, "appears in", {}, GraphPlace{entry, 3, 0.4}) > 0);
        QVERIFY(graph.link(haplo, epubNode, "appears in") > 0); // both nodes have it: one edge after
        graph.logReading(entry, 0, 60, "2026-01-01");
        graph.logReading(epub, 0, 10, "2026-01-01");
        graph.logReading(entry, 60, 70, "2026-01-02");

        QString error;
        QVERIFY2(library.merge(epub, entry, &error), qPrintable(error));
        QVERIFY(!library.book(entry));
        const Book merged = *library.book(epub);
        // Every name it has elsewhere, its own where both had one; not the
        // plugin's mark, since no plugin made this book.
        QCOMPARE(merged.identifiers.value("calibre").toString(), QString("83355555"));
        QCOMPARE(merged.identifiers.value("goodreads").toString(), QString("28507"));
        QCOMPARE(merged.identifiers.value("goodreads:synced").toString(), QString("read|0|"));
        QCOMPARE(merged.identifiers.value("isbn").toString(), QString("9780553562811"));
        QCOMPARE(merged.identifiers.value("google").toString(), QString("mine"));
        QVERIFY(!merged.identifiers.contains("generated"));
        QCOMPARE(library.bookWithIdentifier("goodreads", "28507"), epub);
        // Paper, the shelf further along, the stars and the day.
        QVERIFY(merged.paper);
        QCOMPARE(merged.held(), QString("both"));
        QCOMPARE(merged.shelf, QString("read"));
        QCOMPARE(merged.rating, 4);
        QCOMPARE(merged.finished, QString("2024-03-01"));
        // The further place, with its notes; the bookmark once.
        QCOMPARE(library.progress(epub).percent, 60.0);
        QCOMPARE(library.annotations(epub).size(), 2);
        QCOMPARE(library.recap(epub).text, QString("Later notes."));
        QCOMPARE(library.notes(epub, "cast").text, QString("Who is who."));
        // The graph: one node for the book, with what was met in the other.
        QVERIFY(!graph.node(entryNode));
        QVERIFY(graph.node(epubNode));
        QCOMPARE(graph.seen(haplo).size(), 1);
        QCOMPARE(graph.seen(haplo).first().bookId, epub);
        const GraphView around = graph.around(epubNode);
        QCOMPARE(std::ranges::count_if(around.edges, [&](const GraphEdge &edge) { return edge.source == haplo; }), 1);
        QVERIFY(std::ranges::any_of(graph.ofBook(epub).nodes, [&](const GraphNode &node) { return node.id == haplo; }));
        const QList<ReadingDay> days = graph.readingLog(epub);
        QCOMPARE(days.size(), 2);
        for (const ReadingDay &day : days)
            if (day.day == "2026-01-01")
                QVERIFY(day.from == 0 && day.to == 60);
        QVERIFY(graph.readingLog(entry).isEmpty());
    }

    void aQuestionAboutTheLibraryCarriesWhatItHolds() {
        Book read;
        read.meta.title = "Dune";
        read.meta.authors = QStringList{"Frank Herbert"};
        read.meta.description = QString(2000, 'd');
        read.meta.tags = QStringList{"science fiction"};
        read.percent = 40;
        Book unread;
        unread.meta.title = "Emma";
        const AiRequest request = Curation::libraryQuestion({{read, "Paul has reached the desert."}, {unread, {}}},
                                                            " Which have I started? ");
        const QString asked = request.messages.first().text;
        QVERIFY(asked.contains("\"title\":\"Dune\""));
        QVERIFY(asked.contains("\"read\":\"40%\""));
        QVERIFY(asked.contains("Paul has reached the desert."));
        QVERIFY(asked.contains("\"title\":\"Emma\""));
        // A long description is cut so a whole library fits.
        QVERIFY(!asked.contains(QString(701, 'd')));
        QVERIFY(asked.endsWith("Which have I started?"));
        QVERIFY(request.system.contains("doesn't hold"));
    }

    void aBookNeedNotBeAFile() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "A Paper Book";
        meta.authors = {"Ann Author"};
        const Library::Added paper = library.addEntry(meta, true);
        QVERIFY2(paper.error.isEmpty(), qPrintable(paper.error));
        meta.title = "One For Later";
        const qint64 wished = library.addEntry(meta, false).id;
        QVERIFY(!library.addEntry(BookMeta(), true).error.isEmpty());
        QCOMPARE(library.book(paper.id)->held(), QString("paper"));
        QCOMPARE(library.book(wished)->held(), QString("none"));
        QVERIFY(library.book(paper.id)->formats.isEmpty());
        QCOMPARE(library.books("is:paper").size(), 1);
        QCOMPARE(library.books("is:wishlist").size(), 1);
        QCOMPARE(library.books("is:digital").size(), 0);

        // What is said of its reading, and what another site calls it.
        QCOMPARE(library.book(paper.id)->status(), QString());
        QVERIFY(library.setReading(paper.id, "read", 4, "2026-03-01"));
        QVERIFY(!library.setReading(paper.id, "abandoned", 0, {}));
        QVERIFY(library.setIdentifier(paper.id, "goodreads", "4671"));
        const Book book = *library.book(paper.id);
        QCOMPARE(book.status(), QString("read"));
        QCOMPARE(book.rating, 4);
        QCOMPARE(book.toJson().value("finished").toString(), QString("2026-03-01"));
        QCOMPARE(library.bookWithIdentifier("goodreads", "4671"), paper.id);
        QCOMPARE(library.bookWithIdentifier("goodreads", "1"), qint64(0));
        QCOMPARE(library.books("status:read rating:4").size(), 1);
        QCOMPARE(library.books("status:unread").size(), 1);

        // It becomes digital too once it is given a file, and goes like any other.
        QVERIFY(Fixtures::epub(dir.filePath("one.epub")));
        QVERIFY(library.addFormat(paper.id, dir.filePath("one.epub")));
        QCOMPARE(library.book(paper.id)->held(), QString("both"));
        QVERIFY(library.remove(wished));
        QVERIFY(library.remove(paper.id));
        QVERIFY(library.books().isEmpty());
    }

    void aSeriesKnowsItsEarlierBooks() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        auto entry = [&](const QString &title, double index, const QString &description) {
            BookMeta meta;
            meta.title = title;
            meta.series = index > 0 ? "The Sea" : "";
            meta.seriesIndex = index;
            meta.description = description;
            return library.addEntry(meta, true).id;
        };
        const qint64 one = entry("One", 1, {}), two = entry("Two", 2, "About two."), three = entry("Three", 3, {});
        entry("Four", 4, "Later.");
        entry("Alone", 0, "Elsewhere.");
        QCOMPARE(library.earlierInSeries(three).size(), 2);
        QCOMPARE(library.earlierInSeries(one).size(), 0);
        // Nothing is known of One; Two has only its description.
        QList<SeriesNotes> known = library.seriesNotes(three, "recap");
        QCOMPARE(known.size(), 1);
        QVERIFY(known.first().blurb);
        // A looked-up blurb fills One in, and real notes outrank blurbs.
        library.setNotes(one, "blurb", {0, 0, "Looked up.", 0});
        library.setNotes(two, "cast", {0, 1, "Ann: the captain.", 0});
        known = library.seriesNotes(three, "recap");
        QCOMPARE(known.size(), 2);
        QCOMPARE(known.at(0).text, QString("Looked up."));
        QVERIFY(known.at(0).blurb);
        QCOMPARE(known.at(1).text, QString("Ann: the captain."));
        QVERIFY(!known.at(1).blurb);
    }

    void aLibraryIsForgottenOrThrownAway() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const QString first = dir.filePath("First"), second = dir.filePath("Second"), calibre = dir.filePath("Calibre");
        for (const QString &folder : {first, second}) {
            Library library(folder);
            QVERIFY(library.open());
            settings.setLibraryPath(folder);
        }
        // The default library, never made, is not one of them.
        settings.forgetLibrary(QDir::homePath() + "/Books");
        QString error;
        // Calibre's library, opened by mistake: Omabook's empty database sits in it.
        QVERIFY(Fixtures::calibreLibrary(calibre));
        {
            Library mistake(calibre);
            QVERIFY(mistake.open());
        }
        settings.setLibraryPath(calibre);
        QVERIFY(Libraries::isCalibreOnly(calibre));
        QVERIFY(!Libraries::isCalibreOnly(first));
        // It is never thrown away, and forgetting it tidies up after Omabook
        // and moves on to another library.
        QVERIFY(!Libraries::trash(settings, calibre, &error));
        QVERIFY(error.contains("Calibre"));
        QVERIFY(Libraries::forget(settings, calibre, &error));
        QVERIFY(!settings.libraries().contains(calibre));
        QVERIFY(settings.libraryPath() != calibre);
        QVERIFY(!QFileInfo::exists(calibre + "/omabook.db"));
        QVERIFY(QFileInfo::exists(calibre + "/metadata.db"));

        // A library of Omabook's own keeps its folder when forgotten...
        const QString other = settings.libraryPath() == first ? second : first;
        QVERIFY(Libraries::forget(settings, other, &error));
        QVERIFY(QFileInfo::exists(other + "/omabook.db"));
        QVERIFY(!Libraries::forget(settings, other, &error));
        // ...and the last one left can't be forgotten.
        QVERIFY(!Libraries::forget(settings, settings.libraryPath(), &error));
        QVERIFY(error.contains("only library"));
        // Thrown away, the folder goes too (to the trash where there is one).
        settings.setLibraryPath(other);
        const QString gone = settings.libraryPath() == first ? second : first;
        QVERIFY2(Libraries::trash(settings, gone, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(gone));
        QCOMPARE(settings.libraries(), QStringList{other});
    }

    void whatIsReadBecomesAGraph() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "The First Tide";
        meta.authors = {"Ann Author"};
        meta.series = "The Sea";
        meta.seriesIndex = 1;
        meta.tags = {"fantasy"};
        const qint64 first = library.addEntry(meta, true).id;
        meta.title = "The Second Tide";
        meta.seriesIndex = 2;
        const qint64 second = library.addEntry(meta, true).id;
        Graph graph(library);

        // The library itself is in it: books, their author, series and tags.
        graph.syncLibrary();
        const qint64 bookNode = graph.bookNode(first);
        QVERIFY(bookNode > 0);
        QCOMPARE(graph.find("", "author").size(), 1);
        QCOMPARE(graph.find("", "book").size(), 2);
        const GraphView nearBook = graph.around(bookNode, 1);
        QCOMPARE(nearBook.nodes.first().id, bookNode);
        QCOMPARE(nearBook.nodes.size(), 4); // the book, its author, series and tag
        // Asked for again, nothing is made twice.
        graph.syncLibrary();
        QCOMPARE(graph.find("", "author").size(), 1);

        // Things met while reading, each where it was met.
        const qint64 mara = graph.upsert({}, "character", "Mara", "A ferry pilot.", {}, GraphPlace{first, 1, 0.2});
        const qint64 guild = graph.upsert({}, "group", "The Tide Guild", "Keeps the ferry lines.", {}, GraphPlace{first, 1, 0.5});
        const qint64 secret = graph.upsert({}, "concept", "Tide-singing", "The guild's magic.", {}, GraphPlace{first, 6, 0.1});
        QVERIFY(graph.link(mara, guild, "member of", {}, GraphPlace{first, 2, 0}) > 0);
        QVERIFY(graph.link(mara, secret, "can do", "Revealed late.", GraphPlace{first, 6, 0.1}) > 0);
        // The same thing under another name, later, is the same node, and
        // keeps the earlier place it was met at.
        QCOMPARE(graph.upsert({}, "character", "mara", "A ferry pilot who sings tides.", {{"aliases", QJsonArray{"The Pilot"}}},
                              GraphPlace{first, 4, 0}),
                 mara);
        QCOMPARE(graph.named({}, "character", "the pilot"), mara);
        QCOMPARE(graph.node(mara)->summary, QString("A ferry pilot who sings tides."));
        QCOMPARE(graph.link(mara, guild, "member of", "Since childhood."), graph.link(mara, guild, "member of"));
        QVERIFY(graph.link(mara, mara, "knows") == 0);

        // The graph of the book is never ahead of the reader.
        const GraphView early = graph.ofBook(first, GraphPlace{first, 3, 0});
        QCOMPARE(early.nodes.size(), 2);
        QCOMPARE(early.edges.size(), 1);
        const GraphView all = graph.ofBook(first);
        QCOMPARE(all.nodes.size(), 3);
        QCOMPARE(all.edges.size(), 2);
        // The next book of the series meets her again: one node, two books.
        graph.upsert({}, "character", "Mara", {}, {}, GraphPlace{second, 0, 0});
        QCOMPARE(graph.ofBook(second).nodes.size(), 1);
        QCOMPARE(graph.find("ferry").size(), 2);
        QCOMPARE(graph.find("ferry", "character").size(), 1);

        // Two nodes that turn out to be one are folded together.
        const qint64 twin = graph.upsert({}, "character", "Captain Mara Venn");
        graph.link(twin, secret, "teaches");
        QVERIFY(graph.merge(mara, twin));
        QCOMPARE(graph.named({}, "character", "Captain Mara Venn"), mara);
        QCOMPARE(graph.around(mara, 1).edges.size(), 3);

        // A world of its own, copied whole when it forks, apart from the rest.
        const qint64 hero = graph.upsert("world:9", "character", "Ilse");
        graph.link(hero, graph.upsert("world:9", "place", "Greywater"), "born in");
        QVERIFY(graph.copyScope("world:9", "world:10"));
        QVERIFY(!graph.copyScope("world:9", "world:10"));
        QCOMPARE(graph.whole("world:10").nodes.size(), 2);
        QCOMPARE(graph.whole("world:10").edges.size(), 1);
        QVERIFY(graph.named({}, "character", "Ilse") == 0);
        QVERIFY(graph.removeScope("world:9") && !graph.removeScope({}));
        QCOMPARE(graph.scopes(), (QStringList{"", "world:10"}));

        // It goes out as files other tools read.
        for (const QString &format : GraphView::formats())
            QVERIFY2(graph.whole().exported(format).contains("Mara"), qPrintable(format));
        QVERIFY(graph.whole().exported("cypher").startsWith("MERGE (n:Thing"));
        QVERIFY(graph.whole().exported("csv").isNull());

        // The days a book was read on, one row a day.
        graph.logReading(first, 0, 10, "2026-03-01");
        graph.logReading(first, 10, 25, "2026-03-01");
        graph.logReading(first, 25, 40, "2026-03-02");
        QCOMPARE(graph.readingLog(first).size(), 2);
        QCOMPARE(graph.readingLog(first).first().to, 25.0);

        // A book that leaves takes what was only its own with it.
        QVERIFY(library.remove(second));
        QCOMPARE(graph.find("", "book").size(), 1);
        QCOMPARE(graph.ofBook(second).nodes.size(), 0);
        QVERIFY(graph.node(mara).has_value());
    }

    void whatHappensIsLogged() {
        QTemporaryDir dir;
        Log::install("cli", dir.path());
        qCInfo(lcAi).noquote() << "Claude asked: main tier, 120 characters";
        qCWarning(lcPlugins).noquote() << "storyteller: main.js:5: TypeError" << "\nsecond line";
        qCDebug(lcGraph) << "not kept without OMABOOK_DEBUG";
        qCInfo(lcLibrary) << "opened";
        // Read back, newest last, one line each whatever was said.
        const QList<Log::Entry> all = Log::read({}, dir.path());
        QCOMPARE(all.size(), 3);
        QCOMPARE(all.first().category, QString("ai"));
        QCOMPARE(all.first().program, QString("cli"));
        QCOMPARE(all.at(1).level, QString("warning"));
        QVERIFY(all.at(1).message.contains("TypeError") && all.at(1).message.contains("second line"));
        QVERIFY(all.first().when.secsTo(QDateTime::currentDateTime()) < 5);
        // Filtered by level, part, words and number.
        const auto where = [](auto set) {
            Log::Filter filter;
            set(filter);
            return filter;
        };
        QCOMPARE(Log::read(where([](Log::Filter &f) { f.least = "warning"; }), dir.path()).size(), 1);
        QCOMPARE(Log::read(where([](Log::Filter &f) { f.category = "library"; }), dir.path()).size(), 1);
        QCOMPARE(Log::read(where([](Log::Filter &f) { f.words = "typeerror"; }), dir.path()).size(), 1);
        QCOMPARE(Log::read(where([](Log::Filter &f) { f.lines = 1; }), dir.path()).first().category, QString("library"));
        QCOMPARE(Log::read(where([](Log::Filter &f) { f.since = QDateTime::currentDateTime().addSecs(60); }), dir.path()).size(), 0);
        // A full file is set aside, and its lines are still read.
        {
            QFile big(Log::path(dir.path()));
            QVERIFY(big.open(QIODevice::Append));
            big.write(QByteArray(2 * 1024 * 1024 + 10, 'x') + "\n");
        }
        qCInfo(lcSync) << "after the cut";
        QVERIFY(QFileInfo::exists(Log::path(dir.path()) + ".1"));
        QCOMPARE(Log::read(where([](Log::Filter &f) { f.lines = 0; }), dir.path()).size(), 4);
        QCOMPARE(Log::read(where([](Log::Filter &f) { f.lines = 0; }), dir.path()).last().category, QString("sync"));
        // Back to where the other tests expect messages to go.
        Log::install("test", QDir::tempPath() + "/omabook-library-tests-log");
    }

    void whatWasRemovedIsRemembered() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        const QString hash = Library::hashFile(dir.filePath("a.epub"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("a.epub")).id;
        QCOMPARE(library.bookWithHash(hash), id);
        QCOMPARE(library.formatFiles(id).first().hash, hash);
        QVERIFY(!library.wasRemoved(hash));
        QVERIFY(library.remove(id));
        // A sync won't bring it back...
        QVERIFY(library.wasRemoved(hash));
        QCOMPARE(library.removedHashes(), QStringList{hash});
        // ...but adding it by hand means it is wanted again.
        QVERIFY(library.add(dir.filePath("a.epub")).error.isEmpty());
        QVERIFY(!library.wasRemoved(hash));
    }

    // When a book's details and its reading last changed, which a sync goes
    // by: stamped when something changes, never when nothing does, and with
    // the time given when the change came from another computer.
    void changesAreStampedWithTheirTime() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("a.epub")).id;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVERIFY(library.book(id)->detailsAt >= now - 5);
        QCOMPARE(library.book(id)->readingAt, qint64(0));
        QCOMPARE(library.book(id)->toJson().value("details_at").toInteger(), library.book(id)->detailsAt);

        // The same details again, the same picture again, the same name
        // again and the same reading again change no time.
        QVERIFY(library.setDetailsAt(id, 1700000100));
        QVERIFY(library.setMeta(id, library.book(id)->meta));
        QImage cover(30, 40, QImage::Format_RGB32);
        cover.fill(Qt::red);
        QVERIFY(library.setCover(id, cover, 1700000100));
        QVERIFY(library.setCover(id, cover));
        QVERIFY(library.setIdentifier(id, "goodreads", "1", 1700000100));
        QVERIFY(library.setIdentifier(id, "goodreads", "1"));
        QCOMPARE(library.book(id)->detailsAt, qint64(1700000100));
        QVERIFY(library.setReading(id, "read", 3, "2026-01-02"));
        const qint64 read = library.book(id)->readingAt;
        QVERIFY(read >= now - 5);
        QVERIFY(library.setReading(id, "read", 3, "2026-01-02"));
        QVERIFY(library.setPaper(id, false));
        library.setProgress(id, {.place = {.section = 1, .fraction = 0.5}, .percent = 20, .updated = 0});
        QCOMPARE(library.book(id)->readingAt, read);
        QCOMPARE(library.book(id)->detailsAt, qint64(1700000100));

        // A change is stamped with now, or with the time it came with.
        BookMeta meta = library.book(id)->meta;
        meta.description = "Changed.";
        QVERIFY(library.setMeta(id, meta));
        QVERIFY(library.book(id)->detailsAt >= now - 5);
        meta.description = "Changed elsewhere.";
        QVERIFY(library.setMeta(id, meta, nullptr, 1700000200));
        QCOMPARE(library.book(id)->detailsAt, qint64(1700000200));
        QVERIFY(library.setIdentifier(id, "goodreads", "2", 1700000300));
        QCOMPARE(library.book(id)->detailsAt, qint64(1700000300));
        QVERIFY(library.setIdentifier(id, "goodreads", ""));
        QVERIFY(library.book(id)->detailsAt >= now - 5);
        QVERIFY(library.setDetailsAt(id, 1700000100));
        cover.fill(Qt::blue);
        QVERIFY(library.setCover(id, cover));
        QVERIFY(library.book(id)->detailsAt >= now - 5);
    }

    // A removal is remembered with its time; a file another book still
    // holds, as the book a merge kept does, is not taken for removed.
    void aRemovalKeepsItsTimeAndSparesWhatIsStillHere() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub"), "Kept"));
        QVERIFY(Fixtures::write(dir.filePath("b.txt"), "The other one.\n"));
        QVERIFY(Fixtures::epub(dir.filePath("c.epub"), "Gone"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 kept = library.add(dir.filePath("a.epub")).id;
        const qint64 other = library.add(dir.filePath("b.txt")).id;
        const qint64 gone = library.add(dir.filePath("c.epub")).id;
        QVERIFY(library.merge(kept, other));
        QCOMPARE(library.book(kept)->formats, (QStringList{"epub", "txt"}));
        QVERIFY(!library.wasRemoved(Library::hashFile(dir.filePath("b.txt"))));
        QVERIFY(!library.removedAt(Library::hashFile(dir.filePath("b.txt"))));
        QVERIFY(library.remove(gone));
        const QString hash = Library::hashFile(dir.filePath("c.epub"));
        const std::optional<qint64> at = library.removedAt(hash);
        QVERIFY(at && *at >= QDateTime::currentSecsSinceEpoch() - 5);
        QCOMPARE(library.removals().size(), 1);
        QCOMPARE(library.removals().first().key, hash);
        QCOMPARE(library.removals().first().at, *at);
    }

    void anOlderLibraryIsBroughtUpToDate() {
        // A library from before removals were remembered (schema 1).
        QTemporaryDir dir;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "old");
            database.setDatabaseName(dir.filePath("omabook.db"));
            QVERIFY(database.open());
            {
                Library fresh(dir.filePath("fresh"));
                QVERIFY(fresh.open());
            }
            QSqlDatabase model = QSqlDatabase::addDatabase("QSQLITE", "model");
            model.setDatabaseName(dir.filePath("fresh/omabook.db"));
            QVERIFY(model.open());
            QSqlQuery tables(model);
            tables.exec("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%' AND name NOT IN ('removed', 'recaps', 'notes', 'reading_log', 'library_state', 'book_format') AND name NOT LIKE 'graph_%' AND name NOT LIKE 'annotation_words%' AND name NOT LIKE 'collection%' AND type != 'trigger' AND tbl_name NOT IN ('ops', 'snapshot_ops', 'live_ops', 'space_ops')");
            while (tables.next())
                QVERIFY(QSqlQuery(database).exec(tables.value(0).toString()));
            // The columns that came later are not there yet either, nor the change log's tables (left out above) they are added to.
            for (const char *column : {"paper", "shelf", "rating", "finished", "identifiers", "details_at", "reading_at"})
                QVERIFY(QSqlQuery(database).exec(QString("ALTER TABLE books DROP COLUMN %1").arg(column)));
            QVERIFY(QSqlQuery(database).exec("INSERT INTO books (title, added) VALUES ('Kept', 1)"));
            QVERIFY(QSqlQuery(database).exec("PRAGMA user_version = 1"));
            model.close();
            database.close();
        }
        QSqlDatabase::removeDatabase("old");
        QSqlDatabase::removeDatabase("model");

        Library library(dir.path());
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.books().size(), 1);
        QCOMPARE(library.books().first().meta.title, QString("Kept"));
        // Before it was changed it was copied, as it was: format 1, one book.
        const QStringList copies = QDir(library.backupFolder()).entryList({"omabook-format-1-*.db"});
        QCOMPARE(copies.size(), 1);
        {
            QSqlDatabase copy = QSqlDatabase::addDatabase("QSQLITE", "copy");
            copy.setDatabaseName(library.backupFolder() + "/" + copies.first());
            QVERIFY(copy.open());
            QSqlQuery version(copy);
            QVERIFY(version.exec("PRAGMA user_version") && version.next());
            QCOMPARE(version.value(0).toInt(), 1);
            QSqlQuery kept(copy);
            QVERIFY(kept.exec("SELECT title FROM books") && kept.next());
            QCOMPARE(kept.value(0).toString(), QString("Kept"));
            copy.close();
        }
        QSqlDatabase::removeDatabase("copy");
        // Opened again there is nothing to change, so no second copy.
        {
            Library again(dir.path());
            QVERIFY(again.open());
            QCOMPARE(QDir(again.backupFolder()).entryList({"omabook-format-*.db"}).size(), 1);
        }
        QVERIFY(library.removedHashes().isEmpty());
        QVERIFY(!library.wasRemoved("x"));
    }

    void progressAndBookmarksAreKept() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        qint64 id = 0;
        {
            Library library(dir.filePath("Library"));
            QVERIFY(library.open());
            id = library.add(dir.filePath("a.epub")).id;
            library.setProgress(id, {1, 0.25, 62.5});
            Annotation bookmark;
            bookmark.kind = "bookmark";
            bookmark.place = {.section = 1, .fraction = 0.25};
            bookmark.text = "The end.";
            QVERIFY(library.addAnnotation(id, bookmark) > 0);
        }
        // Another opening of the same folder finds it all again.
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QCOMPARE(library.progress(id).place.section, 1);
        QCOMPARE(library.progress(id).place.fraction, 0.25);
        // The AI's notes on the book are kept with it too.
        QVERIFY(library.recap(id).text.isEmpty());
        library.setRecap(id, {1, 0.25, "So far: a test.", 0});
        QCOMPARE(library.recap(id).text, QString("So far: a test."));
        // Notes of another kind sit beside them.
        library.setNotes(id, "cast", {1, 0.25, "Ann: the author.", 0});
        QCOMPARE(library.notes(id, "cast").text, QString("Ann: the author."));
        QCOMPARE(library.notes(id, "recap").text, QString("So far: a test."));
        QCOMPARE(library.recap(id).place.section, 1);
        QVERIFY(library.recap(id).updated > 0);
        QCOMPARE(library.book(id)->percent, 62.5);
        QVERIFY(library.book(id)->lastRead > 0);
        const QList<Annotation> marks = library.annotations(id);
        QCOMPARE(marks.size(), 1);
        QCOMPARE(marks.first().text, QString("The end."));
        library.removeAnnotation(marks.first().id);
        QVERIFY(library.annotations(id).isEmpty());
    }

    void twoOpeningsOfOneLibraryStayInStep() {
        // The window and a command, or the import worker, at the same time.
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        Library window(dir.filePath("Library")), command(dir.filePath("Library"));
        QVERIFY(window.open());
        QVERIFY(command.open());
        const qint64 id = command.add(dir.filePath("a.epub")).id;
        QCOMPARE(window.books().size(), 1);
        window.setProgress(id, {0, 0.5, 25});
        QCOMPARE(command.book(id)->percent, 25.0);
    }

    void searchLooksInTheRightFields() {
        Book book;
        book.meta.title = "Dune Messiah";
        book.meta.authors = QStringList{"Frank Herbert"};
        book.meta.series = "Dune";
        book.meta.tags = QStringList{"science fiction"};
        book.meta.language = "en";
        book.formats = QStringList{"epub", "pdf"};
        QVERIFY(book.matches(""));
        QVERIFY(book.matches("dune"));
        QVERIFY(book.matches("messiah herbert"));
        QVERIFY(!book.matches("messiah asimov"));
        QVERIFY(book.matches("author:frank"));
        QVERIFY(!book.matches("author:dune"));
        QVERIFY(book.matches("tag:fiction format:pdf lang:en"));
        QVERIFY(!book.matches("format:mobi"));
        QVERIFY(book.matches("series:dune title:messiah"));
    }

    void theShelfKeepsItsPlaceWhenNothingItShowsChanged() {
        // The library is read again on every write to its database, the
        // knowledge graph's and the reading place's too; the shelf must not
        // be rebuilt (which scrolls it back) for writes it doesn't show.
        auto make = [](qint64 id, const QString &title, double percent) {
            Book book;
            book.id = id;
            book.meta.title = title;
            book.percent = percent;
            return book;
        };
        BookListModel model;
        model.setSort("title");
        model.setBooks({make(1, "Alpha", 0), make(2, "Beta", 10), make(3, "Gamma", 0)}, "/lib");
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setBooks({make(1, "Alpha", 0), make(2, "Beta", 10), make(3, "Gamma", 0)}, "/lib");
        QCOMPARE(reset.count(), 0);
        QCOMPARE(changed.count(), 0);
        // One book read further: that row is redrawn, nothing else moves.
        model.setBooks({make(1, "Alpha", 0), make(2, "Beta", 40), make(3, "Gamma", 0)}, "/lib");
        QCOMPARE(reset.count(), 0);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.first().first().toModelIndex().row(), 1);
        // A book that comes, or an order that changes, does rebuild the shelf.
        model.setBooks({make(1, "Alpha", 0), make(2, "Beta", 40), make(3, "Gamma", 0), make(4, "Delta", 0)}, "/lib");
        QCOMPARE(reset.count(), 1);
        model.setBooks({make(1, "Alpha", 0), make(2, "Beta", 40), make(3, "Gamma", 0), make(4, "Delta", 0)}, "/other");
        QCOMPARE(reset.count(), 2);
    }

    void theShelfFiltersAndSorts() {
        auto make = [](qint64 id, const QString &title, const QString &author, qint64 lastRead) {
            Book book;
            book.id = id;
            book.meta.title = title;
            book.meta.authors = QStringList{author};
            book.lastRead = lastRead;
            return book;
        };
        BookListModel model;
        model.setBooks({make(1, "Zebra", "Adams", 0), make(2, "apple 10", "Brown", 50), make(3, "Apple 9", "Adams", 0)}, "/lib");
        auto titles = [&] {
            QStringList titles;
            for (int row = 0; row < model.rowCount(); ++row)
                titles << model.data(model.index(row), model.roleNames().key("title")).toString();
            return titles;
        };
        // Recently read first, then the newest.
        QCOMPARE(titles(), (QStringList{"apple 10", "Apple 9", "Zebra"}));
        model.setSort("title");
        QCOMPARE(titles(), (QStringList{"Apple 9", "apple 10", "Zebra"}));
        model.setSort("author");
        QCOMPARE(titles(), (QStringList{"Apple 9", "Zebra", "apple 10"}));
        model.setSort("added");
        QCOMPARE(titles(), (QStringList{"Apple 9", "apple 10", "Zebra"}));

        model.setQuery("apple");
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.total(), 3);
        QCOMPARE(model.rowOf(2), 1);
        QCOMPARE(model.idAt(0), qint64(3));
        QCOMPARE(model.rowOf(1), -1);
    }

    void theShelfSortsEveryWayAndItsFiltersCombine() {
        // Five books that differ in everything the shelf sorts and filters by.
        Book dune, messiah, wizard, diary, zen;
        dune.id = 1;
        dune.meta.title = "Dune";
        dune.meta.authors = QStringList{"Frank Herbert"};
        dune.meta.series = "Dune";
        dune.meta.seriesIndex = 1;
        dune.meta.language = "en";
        dune.meta.tags = QStringList{"SF"};
        dune.meta.published = "1965";
        dune.formats = QStringList{"epub"};
        dune.percent = 100;
        dune.rating = 5;
        dune.finished = "2024-03-01";
        dune.added = 100;
        dune.lastRead = 500;
        messiah.id = 2;
        messiah.meta.title = "Dune Messiah";
        messiah.meta.authors = QStringList{"Frank Herbert"};
        messiah.meta.series = "Dune";
        messiah.meta.seriesIndex = 2;
        messiah.meta.language = "en";
        messiah.meta.tags = QStringList{"SF"};
        messiah.meta.published = "1969";
        messiah.formats = QStringList{"epub", "pdf"};
        messiah.percent = 40;
        messiah.added = 200;
        messiah.lastRead = 900;
        // A paper book, read elsewhere.
        wizard.id = 3;
        wizard.meta.title = "A Wizard of Earthsea";
        wizard.meta.authors = QStringList{"Ursula K. Le Guin"};
        wizard.meta.series = "Earthsea";
        wizard.meta.seriesIndex = 1;
        wizard.meta.language = "en";
        wizard.meta.tags = QStringList{"Fantasy"};
        wizard.meta.published = "1968";
        wizard.paper = true;
        wizard.shelf = "read";
        wizard.rating = 4;
        wizard.finished = "2023-01-15";
        wizard.added = 300;
        // One that is only wished for.
        diary.id = 4;
        diary.meta.title = "Het Achterhuis";
        diary.meta.authors = QStringList{"Anne Frank"};
        diary.meta.language = "nl";
        diary.meta.published = "1947";
        diary.shelf = "to-read";
        diary.added = 400;
        // One held both ways, of which little is known.
        zen.id = 5;
        zen.meta.title = "Zen";
        zen.meta.authors = QStringList{"Robert Pirsig"};
        zen.meta.tags = QStringList{"Philosophy", "Fantasy"};
        zen.formats = QStringList{"pdf"};
        zen.paper = true;
        zen.added = 50;

        BookListModel model;
        model.setBooks({dune, messiah, wizard, diary, zen}, "/lib");
        auto ids = [&] {
            QList<int> ids;
            for (int row = 0; row < model.rowCount(); ++row)
                ids << int(model.idAt(row));
            return ids;
        };
        using Ids = QList<int>;

        // Every order, the way it is most wanted; what has no value for an
        // order comes last, by title.
        QCOMPARE(ids(), (Ids{2, 1, 5, 4, 3}));
        QVERIFY(model.descending());
        model.setSort("title");
        QCOMPARE(ids(), (Ids{3, 1, 2, 4, 5}));
        QVERIFY(!model.descending());
        // By last name: Frank, (Le) Guin, Herbert with the series in order, Pirsig.
        model.setSort("author");
        QCOMPARE(ids(), (Ids{4, 3, 1, 2, 5}));
        model.setSort("series");
        QCOMPARE(ids(), (Ids{1, 2, 3, 4, 5}));
        model.setSort("added");
        QCOMPARE(ids(), (Ids{4, 3, 2, 1, 5}));
        model.setSort("progress");
        QCOMPARE(ids(), (Ids{1, 2, 3, 4, 5}));
        model.setSort("rating");
        QCOMPARE(ids(), (Ids{1, 3, 2, 4, 5}));
        model.setSort("published");
        QCOMPARE(ids(), (Ids{2, 3, 1, 4, 5}));
        model.setSort("finished");
        QCOMPARE(ids(), (Ids{1, 3, 2, 4, 5}));
        model.setSort("no such order");
        QCOMPARE(model.sort(), QString("finished"));

        // Turned round, the order runs the other way, and what has no value
        // still comes last.
        model.setReversed(true);
        QVERIFY(!model.descending());
        QCOMPARE(ids(), (Ids{3, 1, 2, 4, 5}));
        model.setSort("series");
        QCOMPARE(ids(), (Ids{3, 2, 1, 4, 5}));
        model.setSort("title");
        QCOMPARE(ids(), (Ids{5, 4, 2, 1, 3}));
        model.setReversed(false);

        // Each filter on its own.
        auto with = [&](const QString &kind, const QVariant &value) {
            model.clearFilters();
            model.setFilter(kind, value);
            return ids();
        };
        QCOMPARE(with("status", "reading"), (Ids{2}));
        QCOMPARE(with("status", "read"), (Ids{3, 1}));
        QCOMPARE(with("status", "to-read"), (Ids{4}));
        QCOMPARE(with("status", "unread"), (Ids{4, 5}));
        QCOMPARE(with("held", "digital"), (Ids{1, 2, 5}));
        QCOMPARE(with("held", "paper"), (Ids{3, 5}));
        QCOMPARE(with("held", "wishlist"), (Ids{4}));
        QCOMPARE(with("format", "pdf"), (Ids{2, 5}));
        QCOMPARE(with("language", "nl"), (Ids{4}));
        QCOMPARE(with("language", "-"), (Ids{5}));
        QCOMPARE(with("tag", "fantasy"), (Ids{3, 5}));
        QCOMPARE(with("tag", "-"), (Ids{4}));
        QCOMPARE(with("author", "Frank Herbert"), (Ids{1, 2}));
        QCOMPARE(with("series", "Dune"), (Ids{1, 2}));
        QCOMPARE(with("series", "-"), (Ids{4, 5}));
        QCOMPARE(with("rating", 4), (Ids{3, 1}));
        QCOMPARE(with("rating", 5), (Ids{1}));

        // Together they narrow, the search with them; taking one off widens again.
        model.clearFilters();
        model.setFilter("held", "paper");
        model.setFilter("status", "read");
        QCOMPARE(ids(), (Ids{3}));
        QCOMPARE(model.filters(), (QVariantMap{{"held", "paper"}, {"status", "read"}}));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.total(), 5);
        model.setQuery("dune");
        QCOMPARE(model.count(), 0);
        model.setQuery("wizard");
        QCOMPARE(ids(), (Ids{3}));
        model.setQuery({});
        model.setFilter("status", "");
        QCOMPARE(ids(), (Ids{3, 5}));
        // What can't be filtered on is not kept.
        model.setFilters({{"shoe size", 44}, {"rating", 9}});
        QCOMPARE(model.filters(), (QVariantMap{{"rating", 5}}));
        model.clearFilters();
        QCOMPARE(model.count(), 5);

        // What a filter can be set to here, the most common first.
        auto values = [&](const QString &kind) {
            QStringList values;
            for (const QVariant &choice : model.choices(kind))
                values << choice.toMap().value("value").toString() + ":" + choice.toMap().value("count").toString();
            return values;
        };
        QCOMPARE(values("tag"), (QStringList{"Fantasy:2", "SF:2", "Philosophy:1"}));
        QCOMPARE(values("status"), (QStringList{"reading:1", "to-read:1", "read:2", "unread:2"}));
        QCOMPARE(values("held"), (QStringList{"digital:3", "paper:2", "wishlist:1"}));
        QCOMPARE(values("rating"), (QStringList{"5:1", "4:2", "3:2", "2:2", "1:2"}));
        QCOMPARE(values("format"), (QStringList{"epub:2", "pdf:2"}));

        // The same books in piles, the ones with nothing to go by last. A
        // filter of the pile's own kind is left aside, so an open pile
        // doesn't hide the others.
        auto piles = [&](const QString &kind) {
            QStringList piles;
            for (const QVariant &pile : model.groups(kind))
                piles << pile.toMap().value("value").toString() + ":" + pile.toMap().value("count").toString();
            return piles;
        };
        QCOMPARE(piles("series"), (QStringList{"Dune:2", "Earthsea:1", "-:2"}));
        QCOMPARE(piles("author"), (QStringList{"Anne Frank:1", "Ursula K. Le Guin:1", "Frank Herbert:2", "Robert Pirsig:1"}));
        QCOMPARE(piles("status"), (QStringList{"reading:1", "to-read:1", "read:2", "-:1"}));
        model.setFilter("series", "Dune");
        QCOMPARE(piles("series"), (QStringList{"Dune:2", "Earthsea:1", "-:2"}));
        QCOMPARE(piles("tag"), (QStringList{"SF:2"}));
        QCOMPARE(model.groups("series").first().toMap().value("books").toList().first().toMap().value("title").toString(),
                 QString("Dune"));
        model.clearFilters();

        // A row as a view takes it, with the new fields.
        const QVariantMap row = model.get(model.rowOf(3));
        QCOMPARE(row.value("held").toString(), QString("paper"));
        QCOMPARE(row.value("status").toString(), QString("read"));
        QCOMPARE(row.value("rating").toInt(), 4);
        QCOMPARE(row.value("finished").toString(), QString("2023-01-15"));
        QVERIFY(!row.value("spineColor").value<QColor>().isValid());
        QVERIFY(model.get(99).isEmpty());
        QCOMPARE(model.data(model.index(model.rowOf(4)), model.roleNames().key("held")).toString(), QString("none"));
    }

    void theBookshelfStandsTheSameEveryTime() {
        BookListModel model;
        QList<Book> books;
        for (int id = 1; id <= 300; ++id) {
            Book book;
            book.id = id;
            book.meta.title = QString("Book %1").arg(id);
            books << book;
        }
        model.setBooks(books, "/lib");
        const QVariantList shelves = model.shelve(900, 220);
        QVERIFY(shelves.size() > 3);
        QCOMPARE(model.shelve(900, 220), shelves);

        // Every book is on a shelf once, in order, within the shelf's width;
        // most stand, and some lie in piles no higher than a standing book.
        int next = 0, flat = 0;
        for (const QVariant &shelf : shelves) {
            QVERIFY(!shelf.toList().isEmpty());
            for (const QVariant &place : shelf.toList()) {
                const QVariantMap slot = place.toMap();
                QCOMPARE(slot.value("row").toInt(), next++);
                QVERIFY(slot.value("x").toReal() >= 0);
                QVERIFY(slot.value("x").toReal() + slot.value("w").toReal() <= 900);
                QVERIFY(slot.value("y").toReal() + slot.value("h").toReal() <= 220);
                if (slot.value("flat").toBool()) {
                    ++flat;
                    QVERIFY(slot.value("w").toReal() > slot.value("h").toReal());
                } else {
                    QCOMPARE(slot.value("y").toReal(), 0.0);
                    QVERIFY(slot.value("h").toReal() > slot.value("w").toReal());
                }
            }
        }
        QCOMPARE(next, 300);
        QVERIFY2(flat > 10 && flat < 150, qPrintable(QString::number(flat)));
        // A narrower shelf holds fewer, so there are more of them.
        QVERIFY(model.shelve(400, 220).size() > shelves.size());

        // A spine is bound in its cover's main color, not the white of its
        // lettering, and is lettered in what reads on it.
        QImage cover(200, 300, QImage::Format_RGB32);
        cover.fill(QColor("#a02020"));
        for (int y = 40; y < 90; ++y)
            for (int x = 20; x < 180; ++x)
                cover.setPixel(x, y, qRgb(255, 255, 255));
        const QColor color = BookListModel::dominantColor(cover);
        QVERIFY2(color.red() > 140 && color.green() < 60 && color.blue() < 60, qPrintable(color.name()));
        QVERIFY(!BookListModel::dominantColor(QImage()).isValid());
        QVERIFY(model.inkOn(color).lightnessF() > 0.8);
        QVERIFY(model.inkOn(QColor("#f4e9c8")).lightnessF() < 0.2);
    }

    void aBookKeepsAReadingFormatOfItsOwn() {
        QTemporaryDir dir;
        Library library(dir.path());
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "Set My Way";
        const qint64 book = library.addEntry(meta, true).id;
        meta.title = "As Usual";
        const qint64 other = library.addEntry(meta, true).id;
        QVERIFY(library.bookFormat(book).isEmpty());
        const Library::Changes before = library.changes();
        // Only what fits is kept: an option out of its bounds, or one that
        // isn't an option, is left out.
        QVERIFY(library.setBookFormat(book, {{"font_size", 24}, {"align", "center"}, {"line_height", 900}, {"sparkle", true}}));
        QCOMPARE(library.bookFormat(book), (QJsonObject{{"font_size", 24}, {"align", "center"}}));
        QVERIFY(library.bookFormat(other).isEmpty());
        // It is counted apart from what the shelf shows.
        const Library::Changes after = library.changes();
        QVERIFY(after.format > before.format);
        QCOMPARE(after.books, before.books);
        // Written again the same, nothing changes.
        QVERIFY(library.setBookFormat(book, {{"align", "center"}, {"font_size", 24}}));
        QCOMPARE(library.changes().format, after.format);
        // Over the defaults, the book's own win and the rest are the defaults'.
        const QJsonObject defaults{{"font_size", 19}, {"align", "start"}, {"margins", 32}};
        QCOMPARE(ReadingFormat::over(defaults, library.bookFormat(book)),
                 (QJsonObject{{"font_size", 24}, {"align", "center"}, {"margins", 32}}));
        // Reset, it follows the defaults again; a book that isn't there takes none.
        QVERIFY(library.setBookFormat(book, {}));
        QVERIFY(library.bookFormat(book).isEmpty());
        QVERIFY(!library.setBookFormat(9999, {{"indent", 8}}));
        // Folded into another book, its way goes along where that one has none.
        QVERIFY(library.setBookFormat(other, {{"indent", 20}}));
        QVERIFY(library.merge(book, other));
        QCOMPARE(library.bookFormat(book), (QJsonObject{{"indent", 20}}));
        // Gone with its book.
        QVERIFY(library.remove(book));
        QVERIFY(library.bookFormat(book).isEmpty());
    }

    void theReadingFormatIsBoundedAndJustifyStillCounts() {
        QTemporaryDir dir;
        const QString file = dir.filePath("config.json");
        // A config.json from before there was a choice of alignment.
        QVERIFY(Fixtures::write(file, "{\"justify\": true, \"line_height\": 170}"));
        {
            Settings settings(file);
            QCOMPARE(settings.align(), QString("justify"));
            QVERIFY(settings.justify());
            QCOMPARE(settings.lineHeight(), 170);
            // The defaults of what wasn't set, and what each may be.
            QCOMPARE(settings.indent(), 0);
            QCOMPARE(settings.margins(), 32);
            QCOMPARE(settings.format().value("align").toString(), QString("justify"));
            QCOMPARE(settings.format().keys().size(), ReadingFormat::keys().size());
            // Out of bounds is refused by name, or held to the bounds by the setters.
            QVERIFY(!settings.setOption("line_height", 40));
            QVERIFY(!settings.setOption("margins", 500));
            QVERIFY(!settings.setOption("indent", -1));
            QVERIFY(!settings.setOption("font_size", 12.5));
            QVERIFY(!settings.setOption("align", "sideways"));
            QVERIFY(settings.setOption("align", "center"));
            QVERIFY(!settings.justify());
            settings.setMargins(900);
            QCOMPARE(settings.margins(), 160);
            settings.setIndent(-20);
            QCOMPARE(settings.indent(), 0);
            // The old key is still taken, as the alignment.
            QVERIFY(settings.setOption("justify", true));
            QCOMPARE(settings.align(), QString("justify"));
            QVERIFY(settings.setOption("justify", false));
            QCOMPARE(settings.align(), QString("start"));
            QVERIFY(!settings.setOption("justify", "yes"));
        }
        // Saved, the file has the new key and not the old one.
        QFile saved(file);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject written = QJsonDocument::fromJson(saved.readAll()).object();
        QCOMPARE(written.value("align").toString(), QString("start"));
        QVERIFY(!written.contains("justify"));
        QCOMPARE(written.value("config_version").toInt(), 3);
        // What a value may be is said in words, with the range.
        QCOMPARE(ReadingFormat::problem("line_height", 400), QString("line_height is 100 to 300 (percent); 400 is not."));
        QVERIFY(ReadingFormat::problem("align", "middle").contains("start, justify, center or end"));
        QVERIFY(ReadingFormat::problem("margins", 0).isEmpty());
        QVERIFY(ReadingFormat::problem("font_family", "Inter").isEmpty());
        // Every number's range keeps it readable.
        for (const ReadingFormat::Range &range : ReadingFormat::ranges)
            QVERIFY(range.low < range.high && range.fallback >= range.low && range.fallback <= range.high && range.step > 0);
        QVERIFY(ReadingFormat::range(u"font_size")->low >= 10 && ReadingFormat::range(u"measure")->low >= 40
                && ReadingFormat::range(u"measure")->high <= 90 && ReadingFormat::range(u"measure")->fallback == 66);
        QVERIFY(ReadingFormat::range(u"line_height")->low >= 100);
    }

    // The longest line was a width in pixels and is a measure in
    // characters; two pages or one was "spread" and is a count of pages.
    // An older config.json is carried over and written in the new shape.
    void theOldWidthOfALineBecomesAMeasure() {
        QTemporaryDir dir;
        const auto upgraded = [&dir](const QJsonObject &old) {
            const QString file = dir.filePath(QString("config-%1.json").arg(qHash(QJsonDocument(old).toJson())));
            QFile config(file);
            if (!config.open(QIODevice::WriteOnly) || config.write(QJsonDocument(old).toJson()) <= 0)
                return QJsonObject();
            config.close();
            Settings settings(file);
            settings.setSort("title");
            QFile saved(file);
            return saved.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(saved.readAll()).object() : QJsonObject();
        };
        // A width chosen at its size of type: what it held there.
        QJsonObject written = upgraded({{"config_version", 2}, {"line_width", 900}, {"font_size", 20}, {"spread", "always"}});
        QCOMPARE(written.value("measure").toInt(), 90);
        QCOMPARE(written.value("pages").toInt(), 2);
        QVERIFY(!written.contains("line_width") && !written.contains("spread"));
        QCOMPARE(written.value("config_version").toInt(), 3);
        written = upgraded({{"config_version", 2}, {"line_width", 560}, {"spread", "never"}});
        QCOMPARE(written.value("measure").toInt(), 59);
        QCOMPARE(written.value("pages").toInt(), 1);
        // The old default is the new default; "auto" stays automatic.
        written = upgraded({{"line_width", 720}, {"spread", "auto"}});
        QVERIFY(!written.contains("measure"));
        QCOMPARE(written.value("pages").toString(), QString("auto"));
        QCOMPARE(Settings(dir.filePath("none.json")).measure(), 66);
        QCOMPARE(Settings(dir.filePath("none.json")).pages(), 0);
        // A measure already there wins over an older width beside it.
        written = upgraded({{"line_width", 400}, {"measure", 70}});
        QCOMPARE(written.value("measure").toInt(), 70);

        // The settings take the new keys, and the old ones from a plugin.
        Settings settings(dir.filePath("set.json"));
        QVERIFY(settings.setOption("measure", 58));
        QCOMPARE(settings.measure(), 58);
        QVERIFY(!settings.setOption("measure", 120));
        QVERIFY(!settings.setOption("measure", 20));
        QCOMPARE(ReadingFormat::problem("measure", 120), QString("measure is 40 to 90 (characters); 120 is not."));
        QVERIFY(settings.setOption("pages", 3));
        QCOMPARE(settings.pages(), 3);
        QVERIFY(settings.setOption("pages", "2"));
        QCOMPARE(settings.pages(), 2);
        QVERIFY(settings.setOption("pages", "auto"));
        QCOMPARE(settings.pages(), 0);
        QCOMPARE(settings.options().value("pages").toString(), QString("auto"));
        QVERIFY(!settings.setOption("pages", 4));
        QVERIFY(!settings.setOption("pages", "many"));
        QVERIFY(settings.setOption("spread", "always"));
        QCOMPARE(settings.pages(), 2);
        settings.setFontSize(20);
        QVERIFY(settings.setOption("line_width", 800));
        QCOMPARE(settings.measure(), 80);
        QVERIFY(!settings.setOption("line_width", 100));
        QCOMPARE(settings.format().value("measure").toInt(), 80);
        QVERIFY(!settings.format().contains("line_width"));
    }

    // A book's own longest line in pixels, from a library an older Omabook
    // kept, is its own measure in characters once the library is opened.
    void aBooksOwnWidthOfALineBecomesItsMeasure() {
        QTemporaryDir dir;
        {
            QFile file(QFINDTESTDATA("data/library-0.1.3.sql"));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "older");
            database.setDatabaseName(dir.filePath("omabook.db"));
            QVERIFY(database.open());
            QStringList statements = QString::fromUtf8(file.readAll()).split(";\n", Qt::SkipEmptyParts);
            // As 0.1.3 wrote a book's own width: at its own size, and at the default size.
            statements << "UPDATE book_format SET options = '{\"align\":\"justify\",\"font_size\":22,\"line_width\":640}' WHERE book_id = 1"
                       << "INSERT INTO book_format VALUES(2,'{\"line_width\":1000,\"indent\":8}')"
                       << "INSERT INTO book_format VALUES(3,'{\"line_width\":480,\"measure\":70}')";
            for (const QString &statement : statements) {
                const QString sql = statement.trimmed();
                if (sql.isEmpty() || sql == "COMMIT" || sql.startsWith("BEGIN"))
                    continue;
                QSqlQuery query(database);
                QVERIFY2(query.exec(sql), qPrintable(query.lastError().text() + "\n" + sql.left(120)));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase("older");
        Library library(dir.path());
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.bookFormat(1), (QJsonObject{{"align", "justify"}, {"font_size", 22}, {"measure", 58}}));
        QCOMPARE(library.bookFormat(2), (QJsonObject{{"indent", 8}, {"measure", 90}}));
        QCOMPARE(library.bookFormat(3), (QJsonObject{{"measure", 70}}));
    }

    void aChapterIsSetAsTheFormatSays() {
        const QString chapter = "<h1>One</h1><p>First paragraph.</p><p>Second.</p><p align=\"center\">* * *</p><p>Third.</p>";
        const QJsonObject plain{{"line_height", 160}, {"paragraph_spacing", 10}, {"indent", 0}, {"align", "start"}};
        QString set = ReadingFormat::setChapter(chapter, plain, QColor("#123456"));
        QVERIFY(set.contains("line-height: 160%") && set.contains("margin-bottom: 10px") && set.contains("color: #123456"));
        QVERIFY(set.endsWith(chapter));
        QVERIFY(!set.contains("text-align"));
        // Justified and indented: only on the book's plain paragraphs, and
        // the first after the heading is not indented.
        set = ReadingFormat::setChapter(chapter, {{"indent", 20}, {"align", "justify"}}, QColor("#123456"));
        QVERIFY2(set.contains("</h1><p align=\"justify\">First"), qPrintable(set));
        QVERIFY(set.contains("<p align=\"justify\" style=\"text-indent: 20px\">Second"));
        QVERIFY(set.contains("<p align=\"center\">* * *"));
        QVERIFY(set.contains("<p align=\"justify\" style=\"text-indent: 20px\">Third"));
        // Centred and at the end go in the style sheet, which Qt reads against
        // the direction of each paragraph; centred lines get no indent.
        set = ReadingFormat::setChapter(chapter, {{"indent", 20}, {"align", "center"}}, QColor("#123456"));
        QVERIFY(set.contains("text-align: center") && !set.contains("text-indent"));
        set = ReadingFormat::setChapter(chapter, {{"align", "end"}}, QColor("#123456"));
        QVERIFY(set.contains("text-align: right"));
    }

    void settingsAreKeptAndBounded() {
        QTemporaryDir dir;
        const QString file = dir.filePath("deep/config.json");
        {
            Settings settings(file);
            QCOMPARE(settings.libraryPath(), QDir::homePath() + "/Books");
            QCOMPARE(settings.fontSize(), 19);
            settings.setFontSize(500);
            settings.setLibraryPath(dir.filePath("One"));
            settings.setLibraryPath(dir.filePath("Two"));
            settings.setAiProvider("off");
        }
        Settings settings(file);
        QCOMPARE(settings.fontSize(), 48);
        QCOMPARE(settings.libraryPath(), dir.filePath("Two"));
        // The newest first, and the one it started with is not forgotten.
        QCOMPARE(settings.libraries(), (QStringList{dir.filePath("Two"), dir.filePath("One"), QDir::homePath() + "/Books"}));
        QCOMPARE(settings.aiProvider(), QString("off"));
    }

    void aNewerLibraryIsLeftAlone() {
        QTemporaryDir dir;
        {
            Library library(dir.path());
            QVERIFY(library.open());
        }
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "bump");
            database.setDatabaseName(dir.filePath("omabook.db"));
            QVERIFY(database.open());
            QVERIFY(QSqlQuery(database).exec("PRAGMA user_version = 99"));
            database.close();
        }
        QSqlDatabase::removeDatabase("bump");
        Library library(dir.path());
        QString error;
        QVERIFY(!library.open(&error));
        QVERIFY2(error.contains("newer"), qPrintable(error));
        // It says what to do, and where the copies from before an update are.
        QVERIFY2(error.contains("Update Omabook") && error.contains(".backups"), qPrintable(error));
    }

    // Upkeep that runs on every library the user named (Folio's folders,
    // after each command) opens one only as it is: a library in another
    // format is left in it, with no copy made, until it is opened on purpose.
    void upkeepNeverChangesALibrarysFormat() {
        QTemporaryDir dir;
        {
            Library library(dir.path());
            QVERIFY(library.open());
        }
        const auto versionNow = [&] {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "peek");
            database.setDatabaseName(dir.filePath("omabook.db"));
            int version = -1;
            if (database.open()) {
                QSqlQuery query(database);
                if (query.exec("PRAGMA user_version") && query.next())
                    version = query.value(0).toInt();
            }
            database.close();
            return version;
        };
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "older");
            database.setDatabaseName(dir.filePath("omabook.db"));
            QVERIFY(database.open());
            QVERIFY(QSqlQuery(database).exec("PRAGMA user_version = 5"));
            database.close();
        }
        QSqlDatabase::removeDatabase("older");
        {
            Library library(dir.path());
            library.keepFormat();
            QString error;
            QVERIFY(!library.open(&error));
            QVERIFY2(error.contains("format 5"), qPrintable(error));
        }
        QCOMPARE(versionNow(), 5);
        QSqlDatabase::removeDatabase("peek");
        QVERIFY(!QDir(dir.filePath(".backups")).exists() || QDir(dir.filePath(".backups")).isEmpty());
    }

    void everyReleasedLibraryStillOpens() {
        // A library as each release left it, kept as a dump of its database
        // (tests/data/library-<version>.sql). Every one must still open, be
        // brought up to date with nothing lost, and be copied first when its
        // format changes. A new release adds its own dump; none is removed.
        const QDir data(QFileInfo(QFINDTESTDATA("data/library-0.1.0.sql")).absolutePath());
        const QStringList dumps = data.entryList({"library-*.sql"}, QDir::Files, QDir::Name);
        QVERIFY(dumps.contains("library-0.1.0.sql"));
        for (const QString &dump : dumps) {
            QTemporaryDir dir;
            int format = 0;
            {
                QFile file(data.filePath(dump));
                QVERIFY(file.open(QIODevice::ReadOnly));
                QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "released");
                database.setDatabaseName(dir.filePath("omabook.db"));
                QVERIFY(database.open());
                for (const QString &statement : QString::fromUtf8(file.readAll()).split(";\n", Qt::SkipEmptyParts)) {
                    const QString sql = statement.trimmed();
                    if (sql.isEmpty() || sql == "COMMIT" || sql.startsWith("BEGIN"))
                        continue;
                    QSqlQuery query(database);
                    QVERIFY2(query.exec(sql), qPrintable(dump + ": " + query.lastError().text() + "\n" + sql.left(120)));
                }
                QSqlQuery version(database);
                QVERIFY(version.exec("PRAGMA user_version") && version.next());
                format = version.value(0).toInt();
                version.finish();
                database.close();
            }
            QSqlDatabase::removeDatabase("released");
            QVERIFY2(format > 0, qPrintable(dump));

            Library library(dir.path());
            QString error;
            QVERIFY2(library.open(&error), qPrintable(dump + ": " + error));
            QVERIFY(library.formatVersion() >= format);
            // A change of format leaves a copy of the library as it was.
            const QStringList copies = QDir(library.backupFolder()).entryList({QString("omabook-format-%1-*.db").arg(format)});
            QCOMPARE(copies.size(), library.formatVersion() > format ? 1 : 0);

            // What 0.1.0 held, which every later Omabook must still show.
            const QList<Book> books = library.books();
            QCOMPARE(books.size(), 3);
            const Book wall = *library.book(1), paper = *library.book(2), later = *library.book(3);
            QCOMPARE(wall.meta.title, QString("The Sea Wall"));
            QCOMPARE(wall.meta.series, QString("The Coast"));
            QCOMPARE(wall.meta.tags, (QStringList{"fiction", "sea"}));
            QCOMPARE(wall.status(), QString("reading"));
            QCOMPARE(wall.rating, 4);
            QCOMPARE(qRound(wall.percent), 60);
            QCOMPARE(library.progress(1).place.section, 1);
            QCOMPARE(library.annotations(1).size(), 1);
            QCOMPARE(paper.held(), QString("paper"));
            QCOMPARE(paper.status(), QString("read"));
            QCOMPARE(paper.finished, QString("2026-03-01"));
            QCOMPARE(later.held(), QString("none"));
            QCOMPARE(later.status(), QString("to-read"));
            const Graph graph(library);
            const qint64 ann = graph.named({}, "character", "Ann");
            QVERIFY(ann > 0);
            QCOMPARE(graph.node(ann)->summary, QString("Walks the sea wall."));
            QCOMPARE(graph.around(ann, 1).edges.size(), 1);
            QCOMPARE(graph.ofBook(1).nodes.size(), 2);
            QVERIFY(graph.scopes().contains("world:1"));
            QCOMPARE(graph.readingLog(1).size(), 1);
            // From 0.1.3, a book's own reading format, which an older
            // library has none of and can be given after the upgrade.
            if (format >= 8)
                QCOMPARE(library.bookFormat(1), (QJsonObject{{"align", "justify"}, {"font_size", 22}, {"indent", 16}}));
            else
                QVERIFY(library.bookFormat(1).isEmpty());
            QVERIFY(library.setBookFormat(2, {{"line_height", 180}}));
            QCOMPARE(library.bookFormat(2).value("line_height").toInt(), 180);
            // From 0.1.4, what was learned of each thing is kept with the
            // moment it was revealed: in an older library, from where the
            // thing was first met. And how far each book was ever read.
            const auto summaryOf = [&](const GraphPlace &at) {
                for (const GraphNode &node : graph.ofBook(1, at).nodes)
                    if (node.id == ann)
                        return node.summary;
                return QString();
            };
            const QList<GraphFact> facts = graph.facts(ann);
            QVERIFY2(std::ranges::any_of(facts, [](const GraphFact &fact) {
                         return fact.kind == "summary" && fact.value == "Walks the sea wall." && fact.revealed.place.bookId == 1
                                && fact.revealed.place.section == 1;
                     }), qPrintable(dump));
            QCOMPARE(summaryOf({1, 1, 0.5}), QString("Walks the sea wall."));
            QCOMPARE(library.notes(1, "furthest").place.section, 1);
            // A later summary is known from where it was revealed, and not before.
            if (format >= 10) {
                const qint64 later = graph.named({}, "character", "Ann");
                QVERIFY(std::ranges::any_of(graph.facts(later), [](const GraphFact &fact) { return !fact.revealed.anchor.isEmpty(); }));
            }
            Graph(library).learn(ann, "summary", "Walks the sea wall, and keeps the lamps.", GraphMoment{{1, 1, 0.8}, "The lamps"});
            QCOMPARE(summaryOf({1, 1, 0.6}), QString("Walks the sea wall."));
            QCOMPARE(summaryOf({1, 1, 0.9}), QString("Walks the sea wall, and keeps the lamps."));
            // Going back to an earlier page leaves the furthest place where it was.
            library.setProgress(1, {.place = {.section = 0, .fraction = 0.2}, .percent = 10, .updated = 0});
            QCOMPARE(library.notes(1, "furthest").place.section, 1);
            library.setProgress(1, {.place = {.section = 2, .fraction = 0.1}, .percent = 80, .updated = 0});
            QCOMPARE(library.notes(1, "furthest").place.section, 2);
            // From 0.1.4, collections, one inside another: a 0.1.4 library
            // keeps them; an older one has none and can be given some.
            if (format >= 11) {
                const auto stories = library.collectionNamed("Stories");
                const auto coast = library.collectionNamed("Stories/The Coast");
                QVERIFY2(stories && coast, qPrintable(dump));
                QCOMPARE(coast->parent, stories->id);
                QCOMPARE(stories->of(library.books()).size(), 2);
                QCOMPARE(coast->of(library.books()).size(), 1);
                QCOMPARE(stories->format.value("align").toString(), QString("justify"));
            } else {
                QVERIFY(library.collections().isEmpty());
            }
            QVERIFY(library.addCollection("Later/Inside", "tag:sea"));
            QCOMPARE(library.collectionNamed("Later")->of(library.books()).size(), 1);
            // From format 12, when the details and the reading last changed:
            // not known for any book of an older library, and kept from now on.
            if (format < 12) {
                QCOMPARE(wall.detailsAt, qint64(0));
                QCOMPARE(paper.readingAt, qint64(0));
            }
            BookMeta changed = wall.meta;
            changed.description = "Along the sea wall.";
            QVERIFY(library.setMeta(1, changed));
            QVERIFY(library.book(1)->detailsAt > 0);
        }
    }

    // The search language collections are filled by: quotes, -word, OR,
    // genre: and the other fields, and a rule that can't be read said so.
    void aRuleIsASearch() {
        Book book;
        book.id = 1;
        book.meta.title = "The Brass Seedling";
        book.meta.authors = QStringList{"Tobias Lindqvist"};
        book.meta.tags = QStringList{"Science Fiction", "Clockwork"};
        book.meta.publisher = "Harbour Press";
        book.meta.description = "A garden that keeps time.";
        book.formats = QStringList{"epub"};
        QVERIFY(book.matches("tag:\"science fiction\""));
        QVERIFY(!book.matches("tag:\"science fact\""));
        QVERIFY(book.matches("genre:fiction"));
        QVERIFY(!book.matches("genre:nonfiction"));
        QVERIFY(book.matches("-tag:poetry brass"));
        QVERIFY(!book.matches("-tag:clockwork"));
        QVERIFY(book.matches("tag:poetry OR author:lindqvist"));
        QVERIFY(!book.matches("tag:poetry OR author:okafor"));
        QVERIFY(book.matches("publisher:harbour about:garden format:epub"));
        QVERIFY(book.matches(""));
        Book essay;
        essay.id = 2;
        essay.meta.title = "Notes on Slow Rivers";
        essay.meta.tags = QStringList{"Nature", "Essays"};
        QVERIFY(essay.matches("genre:nonfiction"));
        QVERIFY(!essay.matches("genre:fiction"));
        Book history;
        history.meta.tags = QStringList{"Historical Fiction"};
        QVERIFY(history.matches("genre:fiction"));
        QVERIFY(!history.matches("genre:nonfiction"));
        QVERIFY(Book::searchProblem("auther:x").contains("auther:"));
        QVERIFY(Book::searchProblem("tag:\"open").contains("quote"));
        QVERIFY(Book::searchProblem("Dune -tag:x OR genre:fiction").isEmpty());
    }

    // A collection: its rule, the books put in and taken out by hand (which
    // win), books that arrive later, names of any kind and language, and
    // collections inside collections.
    void collectionsHoldWhatTheirRulesAndHandsSay() {
        QTemporaryDir dir;
        Library library(dir.path());
        QVERIFY(library.open());
        const auto add = [&](const QString &title, const QStringList &tags) {
            BookMeta meta;
            meta.title = title;
            meta.tags = tags;
            return library.addEntry(meta, true).id;
        };
        const qint64 pipes = add("Pipe Networks", {"Hydraulics"});
        const qint64 soil = add("Soil Mechanics", {"Engineering"});
        const qint64 novel = add("De Zeemuur", {"Roman"});
        const auto study = library.addCollection("Studie/Hydraulica", "tag:hydraulics");
        QVERIFY(study);
        const auto werk = library.collectionNamed("Studie");
        QVERIFY(werk);
        QCOMPARE(library.collection(*study)->parent, werk->id);
        QCOMPARE(library.collection(*study)->path, QString("Studie/Hydraulica"));
        // Unique among neighbours only; "/" is no name.
        QVERIFY(!library.addCollection("Hydraulica", {}, werk->id));
        QVERIFY(library.addCollection("Plezier/Hydraulica"));
        QVERIFY(!library.addCollection("Studie/"));
        QVERIFY(!library.renameCollection(*study, "a/b"));
        QVERIFY(!library.collectionNamed("Hydraulica")); // two have that name
        QVERIFY(!library.addCollection("Bad", "auther:x"));

        // The parent holds what is inside it.
        const QList<Book> books = library.books();
        QCOMPARE(library.collection(werk->id)->of(books).size(), 1);
        // By hand: in, and out, whatever the rule.
        QVERIFY(library.putInCollection(*study, soil, true));
        QVERIFY(library.collection(*study)->included.contains(soil));
        QVERIFY(library.putInCollection(*study, pipes, false));
        QVERIFY(library.collection(*study)->excluded.contains(pipes));
        QVERIFY(!library.collection(*study)->contains(*library.book(pipes)));
        // Taken back in, it follows the rule again: nothing is kept for it.
        QVERIFY(library.putInCollection(*study, pipes, true));
        QVERIFY(!library.collection(*study)->excluded.contains(pipes) && !library.collection(*study)->included.contains(pipes));
        // A parent's own "out" wins over what is inside it.
        QVERIFY(library.putInCollection(werk->id, soil, false));
        QVERIFY(!library.collection(werk->id)->contains(*library.book(soil)));
        QVERIFY(library.collection(*study)->contains(*library.book(soil)));
        // A new book lands in every collection whose rule finds it.
        const qint64 later = add("Open Channel Flow", {"Hydraulics"});
        QVERIFY(library.collection(*study)->contains(*library.book(later)));
        QVERIFY(library.collection(werk->id)->contains(*library.book(later)));
        QVERIFY(!library.collection(*study)->contains(*library.book(novel)));
        QCOMPARE(library.collectionsOf(*library.book(later)).size(), 2);

        // Renaming a parent takes what is inside it along; moving can't make a loop.
        QVERIFY(library.renameCollection(werk->id, "Werk"));
        QVERIFY(library.collectionNamed("Werk/Hydraulica"));
        QVERIFY(!library.setCollectionParent(werk->id, *study));
        QVERIFY(library.setCollectionParent(*study, 0));
        QCOMPARE(library.collection(*study)->depth, 0);
        QVERIFY(library.setCollectionParent(*study, werk->id));
        // Removing a parent keeps the books and moves what was inside it up.
        QVERIFY(library.removeCollection(werk->id));
        QVERIFY(!library.collection(werk->id));
        QCOMPARE(library.collection(*study)->parent, 0);
        QCOMPARE(library.books().size(), 4);
        // A removed name can be made again, empty.
        const auto again = library.addCollection("Werk");
        QVERIFY(again);
        QCOMPARE(*again, werk->id);
        QVERIFY(library.collection(*again)->excluded.isEmpty());
        // A book removed from the library leaves its collections too.
        QVERIFY(library.putInCollection(*again, novel, true));
        QVERIFY(library.remove(novel));
        QVERIFY(library.collection(*again)->included.isEmpty());
    }

    // The AI's sorting, from a stand-in's answer: a collection there is, a
    // new one anywhere in the tree, and books left for the person; only
    // books that were asked about, and nothing changed until applied.
    void theAiProposesCollectionsNewOnesAndWhatIsYoursToDecide() {
        QTemporaryDir dir;
        Library library(dir.path());
        QVERIFY(library.open());
        const auto add = [&](const QString &title, const QString &tag) {
            BookMeta meta;
            meta.title = title;
            meta.tags = QStringList{tag};
            return library.addEntry(meta, true).id;
        };
        const qint64 kalman = add("Kalman Filtering", "Control");
        const qint64 pipes = add("Pipe Networks", "Hydraulics");
        const qint64 pop = add("A Short History of Nearly Everything", "Science");
        const qint64 placed = add("Learning Python", "Programming");
        const qint64 werk = *library.addCollection("Werk");
        const qint64 python = *library.addCollection("Werk/Python", "tag:programming");
        QList<Collection> collections = library.collections();
        // Placed books aren't asked about; one in a collection with others inside it is.
        QVERIFY(library.putInCollection(werk, kalman, true));
        collections = library.collections();
        const QList<QList<Book>> batches = Curation::uncollected(library.books(), collections, 2);
        QCOMPARE(batches.size(), 2);
        QStringList asked;
        for (const QList<Book> &batch : batches)
            for (const Book &book : batch)
                asked << book.meta.title;
        QVERIFY(asked.contains("Kalman Filtering"));
        QVERIFY(!asked.contains("Learning Python"));
        const QList<Book> books = library.books();
        const AiRequest request = Curation::collectionsRequest(books, collections, books, {"Werk/Regeltechniek"});
        QCOMPARE(request.tier, AiRequest::Quick);
        QVERIFY(request.messages.first().text.contains("Werk/Python"));
        QVERIFY(request.messages.first().text.contains("Werk/Regeltechniek"));
        QVERIFY(!request.system.contains("Fiction") && !request.system.contains("Knowledge"));

        const QString answer = QString("Here: [{\"id\": %1, \"collection\": \"Werk/Regeltechniek\", \"new\": true, \"why\": \"control\"},"
                                       " {\"id\": %2, \"collection\": \"werk/python\"},"
                                       " {\"id\": %3, \"undecided\": true, \"why\": \"popular science\"},"
                                       " {\"id\": %4, \"collection\": \"Werk/Hydraulica\", \"new\": true},"
                                       " {\"id\": 999, \"collection\": \"Werk\"}]")
                                   .arg(kalman).arg(placed).arg(pop).arg(pipes);
        const QList<Proposal> proposals = Curation::collectionsFrom(answer, books, collections);
        QStringList kinds;
        for (const Proposal &proposal : proposals)
            kinds << proposal.kind + ":" + proposal.collection;
        // Learning Python is in Werk/Python already: nothing to propose.
        QCOMPARE(kinds, (QStringList{"collection:Werk/Regeltechniek", "undecided:", "collection:Werk/Hydraulica"}));
        QCOMPARE(proposals.first().collectionId, 0);
        QVERIFY(!library.collectionNamed("Werk/Regeltechniek"));
        // Applied, a new one is made where it was proposed, once.
        QVERIFY(Curation::apply(library, proposals.at(0)));
        QVERIFY(Curation::apply(library, proposals.at(0)));
        QVERIFY(Curation::apply(library, proposals.at(1)));
        const auto control = library.collectionNamed("Werk/Regeltechniek");
        QVERIFY(control);
        QCOMPARE(control->parent, werk);
        QVERIFY(control->included.contains(kalman));
        QCOMPARE(library.collections().size(), 3);
        Q_UNUSED(python);
    }

    // Collections travel in a sync: by path and by the books' file hashes,
    // the one changed last winning, a removal too.
    void collectionsTravelBetweenLibraries() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub"), "Shared Book"));
        Library here(dir.filePath("here")), there(dir.filePath("there"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("a.epub")).id, theirs = there.add(dir.filePath("a.epub")).id;
        QVERIFY(mine > 0 && theirs > 0);
        const qint64 inner = *here.addCollection("Leesvoer/Zomer");
        QVERIFY(here.putInCollection(inner, mine, true));
        QVERIFY(here.setCollectionFormat(inner, {{"align", "justify"}}));
        QCOMPARE(Collections::take(there, Collections::manifest(here)), 2);
        const auto zomer = there.collectionNamed("Leesvoer/Zomer");
        QVERIFY(zomer);
        QVERIFY(zomer->included.contains(theirs));
        QCOMPARE(zomer->format.value("align").toString(), QString("justify"));
        // Nothing newer: nothing changes.
        QCOMPARE(Collections::take(there, Collections::manifest(here)), 0);
        // Removed there later, it goes here too.
        QTest::qWait(1100);
        QVERIFY(there.removeCollection(zomer->id));
        QCOMPARE(Collections::take(here, Collections::manifest(there)), 1);
        QVERIFY(!here.collectionNamed("Leesvoer/Zomer"));
        QVERIFY(here.collectionNamed("Leesvoer"));
    }

    // A merge carries what the folded book was put in by hand.
    void aMergedBookKeepsItsCollections() {
        QTemporaryDir dir;
        Library library(dir.path());
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "Twice";
        const qint64 keep = library.addEntry(meta, true).id, other = library.addEntry(meta, false).id;
        const qint64 shelf = *library.addCollection("Shelf");
        QVERIFY(library.putInCollection(shelf, other, true));
        QVERIFY(library.merge(keep, other));
        QVERIFY(library.collection(shelf)->included.contains(keep));
    }

    void theLayersPointOneWay() {
        // The architecture's one rule, kept by a test: formats and the library
        // know nothing of the window, so the command line, the tests and any
        // future front end can use them alone. Only these files may see QML.
        const QStringList presentation{"backend.h", "backend.cpp", "backendcollections.cpp", "backendlookup.cpp", "backendpicker.cpp", "backendsync.cpp", "backendplugins.cpp", "reader.h", "reader.cpp", "assistant.h", "assistant.cpp",
                                       "booklistmodel.h", "booklistmodel.cpp", "main.cpp", "theme.h", "theme.cpp",
                                       "graphcanvas.h", "graphcanvas.cpp", "graphmodel.h", "graphmodel.cpp", "graphjobs.h", "graphjobs.cpp", "graphtenants.h", "graphtenants.cpp", "graphtypes.cpp",
                                       "readingmarks.h", "readingmarks.cpp", "readingpassages.cpp", "marginnotes.h", "marginnotes.cpp", "annotationlayerview.h", "annotationlayerview.cpp", "readinghighlights.h", "readinghighlights.cpp", "readingink.h", "readingink.cpp", "annotationfinder.cpp", "pageformat.h", "pageformat.cpp", "spacedesk.h", "spacedesk.cpp",
                                       "customization.h", "customization.cpp", "interface.h", "interface.cpp", "librarian.h", "librarian.cpp", "librarianfind.cpp",
                                       "translator.h", "translator.cpp", "filepicker.h", "portalfilepicker.h", "inksurface.h", "inksurface.cpp", "inkselection.cpp",
                                       "portalfilepicker.cpp", "chronologycanvas.h", "chronologycanvas.cpp", "chronologymodel.h", "chronologymodel.cpp", "readingstatsview.h", "readingstatsview.cpp"};
        const QString sources = QFileInfo(QFINDTESTDATA("../src/main.cpp")).absolutePath();
        static const QRegularExpression window("#include\\s+[<\"](QQml|QQuick|QtQuick|QtQml|QAbstractListModel|backend\\.h|reader\\.h|assistant\\.h|booklistmodel\\.h)");
        QStringList problems;
        for (const QFileInfo &file : QDir(sources).entryInfoList({"*.h", "*.cpp"}, QDir::Files)) {
            if (presentation.contains(file.fileName()))
                continue;
            QFile source(file.filePath());
            QVERIFY(source.open(QIODevice::ReadOnly));
            const auto match = window.match(QString::fromUtf8(source.readAll()));
            if (match.hasMatch())
                problems << file.fileName() + " reaches into the window: " + match.captured(0);
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }
};

QTEST_MAIN(LibraryTests)
#include "library_tests.moc"
