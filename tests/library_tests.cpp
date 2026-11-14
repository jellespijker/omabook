#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "booklistmodel.h"
#include "calibre.h"
#include "catalog.h"
#include "curation.h"
#include "fixtures.h"
#include "importer.h"
#include "formats.h"
#include "graph.h"
#include "libraries.h"
#include "library.h"
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
        QCOMPARE(library.progress(id).section, 0);
        QVERIFY(QFile::exists(dir.filePath("a.epub")));
        QString error;
        QVERIFY(!library.remove(id, &error));
        QVERIFY(!error.isEmpty());
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
            tables.exec("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%' AND name NOT IN ('removed', 'recaps', 'notes', 'reading_log', 'library_state') AND name NOT LIKE 'graph_%' AND type != 'trigger'");
            while (tables.next())
                QVERIFY(QSqlQuery(database).exec(tables.value(0).toString()));
            // The columns that came later are not there yet either.
            for (const char *column : {"paper", "shelf", "rating", "finished", "identifiers"})
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
            bookmark.section = 1;
            bookmark.fraction = 0.25;
            bookmark.text = "The end.";
            QVERIFY(library.addAnnotation(id, bookmark) > 0);
        }
        // Another opening of the same folder finds it all again.
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QCOMPARE(library.progress(id).section, 1);
        QCOMPARE(library.progress(id).fraction, 0.25);
        // The AI's notes on the book are kept with it too.
        QVERIFY(library.recap(id).text.isEmpty());
        library.setRecap(id, {1, 0.25, "So far: a test.", 0});
        QCOMPARE(library.recap(id).text, QString("So far: a test."));
        // Notes of another kind sit beside them.
        library.setNotes(id, "cast", {1, 0.25, "Ann: the author.", 0});
        QCOMPARE(library.notes(id, "cast").text, QString("Ann: the author."));
        QCOMPARE(library.notes(id, "recap").text, QString("So far: a test."));
        QCOMPARE(library.recap(id).section, 1);
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
            QCOMPARE(library.progress(1).section, 1);
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
        }
    }

    void theLayersPointOneWay() {
        // The architecture's one rule, kept by a test: formats and the library
        // know nothing of the window, so the command line, the tests and any
        // future front end can use them alone. Only these files may see QML.
        const QStringList presentation{"backend.h", "backend.cpp", "reader.h", "reader.cpp", "assistant.h", "assistant.cpp",
                                       "booklistmodel.h", "booklistmodel.cpp", "main.cpp", "theme.h", "theme.cpp",
                                       "graphcanvas.h", "graphcanvas.cpp", "graphmodel.h", "graphmodel.cpp", "graphjobs.h", "graphjobs.cpp",
                                       "customization.h", "customization.cpp", "interface.h", "interface.cpp", "librarian.h", "librarian.cpp",
                                       "translator.h", "translator.cpp", "filepicker.h", "portalfilepicker.h",
                                       "portalfilepicker.cpp"};
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
