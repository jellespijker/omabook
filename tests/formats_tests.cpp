#include <QDir>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include "bookarchive.h"
#include "fixtures.h"
#include "booksource.h"
#include "formatprovider.h"
#include "formatreaders.h"
#include "formats.h"
#include "htmlclean.h"

// The format layer: every kind of book read from a file made on the spot.
class FormatsTests : public QObject {
    Q_OBJECT

private slots:
    void cleanHtmlKeepsStructureAndDropsTheRest() {
        const QString html = cleanHtml("<html><head><title>T</title><style>p{}</style></head><body>"
                                       "<section><h1>Head</h1><p>One <em>two</em> <strong>three</strong>"
                                       "<script>bad()</script><font color=red>four</font></p></section></body></html>");
        QCOMPARE(html, QString("<div><h1>Head</h1><p>One <i>two</i> <b>three</b>four</p></div>"));
    }

    void cleanHtmlForgivesBrokenMarkup() {
        // An unclosed paragraph ends where the next begins, a stray "<" is
        // text, and a close with no open is ignored.
        QCOMPARE(cleanHtml("<p>a<p>b</i> 1 < 2"), QString("<p>a</p><p>b 1 &lt; 2</p>"));
        QCOMPARE(cleanHtml("<b>bold <i>both</b> after"), QString("<b>bold <i>both</i></b> after"));
    }

    void cleanHtmlShowsWhatStylesheetsSay() {
        CleanOptions options;
        options.styles = classStyles(".it{font-style:italic} p.c, .other {text-align:center} .deep p{font-weight:bold}");
        QCOMPARE(options.styles.value("it"), TextStyles(TextStyle::Italic));
        QCOMPARE(options.styles.value("c"), TextStyles(TextStyle::Center));
        QVERIFY(!options.styles.contains("deep"));
        QCOMPARE(cleanHtml("<p class=\"c\"><span class='it'>x</span></p>", options),
                 QString("<p align=\"center\"><i>x</i></p>"));
        QCOMPARE(cleanHtml("<span style=\"font-weight: 700\">y</span>"), QString("<b>y</b>"));
    }

    void cleanHtmlKeepsAnchorsAndAsksAboutLinksAndPictures() {
        CleanOptions options;
        options.link = [](const QString &target) { return target == "keep" ? QString("omabook:1#x") : QString(); };
        options.image = [](const QString &source) { return "src=\"" + source + "\""; };
        QCOMPARE(cleanHtml("<p id=\"n\"><a href=\"keep\">k</a><a href=\"drop\">d</a><img src=\"a.png\"/></p>", options),
                 QString("<p><a name=\"n\"></a><a href=\"omabook:1#x\">k</a>d<img src=\"a.png\"></p>"));
        // Without anyone to ask, a picture is dropped.
        QCOMPARE(cleanHtml("<img src=\"a.png\">text"), QString("text"));
    }

    void textIsDecodedByDeclarationThenUtf8ThenWestern() {
        QCOMPARE(decodeText("caf\xc3\xa9"), QString::fromUtf8("café"));
        QCOMPARE(decodeText("caf\xe9"), QString::fromUtf8("café"));
        QCOMPARE(decodeText("<?xml version=\"1.0\" encoding=\"windows-1251\"?><p>\xcf\xf0\xe8\xe2\xe5\xf2</p>"),
                 QString::fromUtf8("<?xml version=\"1.0\" encoding=\"windows-1251\"?><p>Привет</p>"));
    }

    void archiveListsAndReads() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::zip(dir.filePath("a.zip"), {{"one.txt", "1"}, {"dir/two ü.txt", "22"}, {"empty", ""}}));
        const Archive archive(dir.filePath("a.zip"));
        QCOMPARE(archive.entries(), (QStringList{"one.txt", QString::fromUtf8("dir/two ü.txt"), "empty"}));
        QCOMPARE(archive.read(QString::fromUtf8("dir/two ü.txt")), QByteArray("22"));
        QVERIFY(!archive.read("empty").isNull());
        QVERIFY(archive.read("missing").isNull());
        QVERIFY(Archive(dir.filePath("none.zip")).entries().isEmpty());
    }

    void epubTellsWhoItIs() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub")));
        QString error;
        const auto book = Formats::open(dir.filePath("book.epub"), &error);
        QVERIFY2(book, qPrintable(error));
        const BookMeta meta = book->meta();
        QCOMPARE(meta.title, QString("The Test Book"));
        QCOMPARE(meta.authors, (QStringList{"Ann Author", "Bo Writer"}));
        QCOMPARE(meta.language, QString("en"));
        QCOMPARE(meta.publisher, QString("Fixture Press"));
        QCOMPARE(meta.isbn, QString("9780000000002"));
        QCOMPARE(meta.description, QString("A book made for tests."));
        QCOMPARE(meta.tags, (QStringList{"Testing", "Fixtures"}));
        QCOMPARE(meta.published, QString("2020-05-17"));
        QCOMPARE(meta.series, QString("The Fixtures"));
        QCOMPARE(meta.seriesIndex, 2.0);
        QCOMPARE(meta.cover.size(), QSize(60, 90));
    }

    void epubReadsAsChapters() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub")));
        const auto book = Formats::open(dir.filePath("book.epub"));
        QVERIFY(book);
        QCOMPARE(book->kind(), Document::Flow);
        QCOMPARE(book->count(), 2);

        const QList<TocEntry> toc = book->toc();
        QCOMPARE(toc.size(), 3);
        QCOMPARE(toc[0].title, QString("One"));
        QCOMPARE(toc[1].title, QString("A note"));
        QCOMPARE(toc[1].section, 1);
        QCOMPARE(toc[1].anchor, QString("note"));
        QCOMPARE(toc[1].level, 1);
        QCOMPARE(toc[2].level, 0);

        const QString first = book->html(0, 400);
        QVERIFY2(first.contains("<h1><a name=\"top\"></a>Chapter One</h1>"), qPrintable(first));
        // The stylesheet's italic and centered bold classes survive as markup.
        QVERIFY2(first.contains("<i>dark</i>"), qPrintable(first));
        QVERIFY2(first.contains("<p align=\"center\"><b>Centered words.</b></p>"), qPrintable(first));
        QVERIFY(!first.contains("alert"));
        QVERIFY(!first.contains("color: red"));
        // A link to another chapter, a web link, and one that leads nowhere.
        QVERIFY2(first.contains("<a href=\"omabook:1#note\">the note</a>"), qPrintable(first));
        QVERIFY(first.contains("<a href=\"https://example.com/\">the web</a>"));
        QVERIFY(first.contains("and nowhere."));

        // The picture is scaled to the page and found through its address.
        const auto image = QRegularExpression("<img src=\"image://book/([^\"]+)\" width=\"400\" height=\"200\">").match(first);
        QVERIFY2(image.hasMatch(), qPrintable(first));
        QCOMPARE(bookImageName(image.captured(1)), QString("OEBPS/images/wide map.png"));
        QCOMPARE(QImage::fromData(book->resource("OEBPS/images/wide map.png")).size(), QSize(1200, 600));

        QCOMPARE(book->text(1), QString("Chapter Two\nThe end.\nA note, unclosed."));
        QVERIFY(book->html(2, 400).isEmpty());
    }

    void epubWithANavigationDocument() {
        QTemporaryDir dir;
        Fixtures::Entries entries = Fixtures::epubEntries();
        for (auto &entry : entries)
            if (entry.first == "OEBPS/content.opf")
                entry.second.replace("<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>",
                                     "<item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"/>");
        entries.append(qMakePair(QString("OEBPS/nav.xhtml"), QByteArray(
                        "<html xmlns:epub=\"http://www.idpf.org/2007/ops\"><body><nav epub:type=\"toc\"><ol>"
                        "<li><a href=\"text/ch1.xhtml\">First <i>part</i></a><ol><li><a href=\"text/ch2.xhtml#note\">Inner</a></li></ol></li>"
                        "</ol></nav><nav epub:type=\"landmarks\"><ol><li><a href=\"text/ch2.xhtml\">Skip</a></li></ol></nav>"
                        "</body></html>")));
        QVERIFY(Fixtures::zip(dir.filePath("three.epub"), entries));
        const auto book = Formats::open(dir.filePath("three.epub"));
        QVERIFY(book);
        const QList<TocEntry> toc = book->toc();
        QCOMPARE(toc.size(), 2);
        QCOMPARE(toc[0].title, QString("First part"));
        QCOMPARE(toc[1].title, QString("Inner"));
        QCOMPARE(toc[1].level, 1);
    }

    void lockedAndBrokenEpubsSayWhy() {
        QTemporaryDir dir;
        Fixtures::Entries locked = Fixtures::epubEntries();
        locked.append(qMakePair(QString("META-INF/rights.xml"), QByteArray("<rights/>")));
        QVERIFY(Fixtures::zip(dir.filePath("locked.epub"), locked));
        QString error;
        QVERIFY(!Formats::open(dir.filePath("locked.epub"), &error));
        QVERIFY2(error.contains("DRM"), qPrintable(error));

        QVERIFY(Fixtures::write(dir.filePath("broken.epub"), "not a zip"));
        QVERIFY(!Formats::open(dir.filePath("broken.epub"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!Formats::open(dir.filePath("missing.epub"), &error));
        QVERIFY(error.contains("no such file"));
    }

    void fictionBookReads() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("book.fb2"), Fixtures::fb2()));
        QString error;
        const auto book = Formats::open(dir.filePath("book.fb2"), &error);
        QVERIFY2(book, qPrintable(error));
        const BookMeta meta = book->meta();
        QCOMPARE(meta.title, QString("Fiction Fixture"));
        // The scanner in document-info is not an author of the book.
        QCOMPARE(meta.authors, QStringList{"Lev Tester"});
        QCOMPARE(meta.tags, QStringList{"sf history"});
        QCOMPARE(meta.description, QString("About this book."));
        QCOMPARE(meta.language, QString("ru"));
        QCOMPARE(meta.series, QString("Cycle"));
        QCOMPARE(meta.seriesIndex, 3.0);
        QCOMPARE(meta.publisher, QString("Samizdat"));
        QCOMPARE(meta.cover.size(), QSize(40, 60));

        // Two chapters and the notes.
        QCOMPARE(book->count(), 3);
        QCOMPARE(book->toc().size(), 3);
        QCOMPARE(book->toc()[0].title, QString("First"));
        QCOMPARE(book->toc()[2].title, QString("Notes"));
        const QString first = book->html(0, 300);
        QVERIFY2(first.contains("<h2>First<br></h2>"), qPrintable(first));
        QVERIFY2(first.contains("Opening <i>words</i><a href=\"omabook:2#n1\">[1]</a>."), qPrintable(first));
        QVERIFY2(first.contains("A line<br>Another<br>"), qPrintable(first));
        QVERIFY2(book->html(1, 300).contains("<img src=\"image://book/"), qPrintable(book->html(1, 300)));
        QVERIFY2(book->html(2, 300).contains("<a name=\"n1\"></a>"), qPrintable(book->html(2, 300)));
        QVERIFY(book->text(2).contains("The footnote."));

        // The same book zipped, as it is usually passed around.
        QVERIFY(Fixtures::zip(dir.filePath("book.fb2.zip"), {{"inner.fb2", Fixtures::fb2()}}));
        QCOMPARE(Formats::extension(dir.filePath("book.fb2.zip")), QString("fb2.zip"));
        QCOMPARE(Formats::readMeta(dir.filePath("book.fb2.zip")).title, QString("Fiction Fixture"));
    }

    void plainTextIsCutIntoChapters() {
        QTemporaryDir dir;
        QByteArray text = "The Title\n\n";
        for (int chapter = 1; chapter <= 3; ++chapter) {
            text += "CHAPTER " + QByteArray::number(chapter) + "\n\n";
            for (int i = 0; i < 40; ++i)
                text += "A line of the story that was wrapped\nby a typewriter & goes <on>.\n\n";
        }
        QVERIFY(Fixtures::write(dir.filePath("Story - Sam Scribe.txt"), text));
        const auto book = Formats::open(dir.filePath("Story - Sam Scribe.txt"));
        QVERIFY(book);
        QCOMPARE(book->count(), 3);
        QCOMPARE(book->toc().size(), 3);
        QCOMPARE(book->toc()[1].title, QString("CHAPTER 2"));
        QCOMPARE(book->toc()[1].section, 1);
        const QString second = book->html(1, 0);
        QVERIFY(second.startsWith("<h2>CHAPTER 2</h2>"));
        QVERIFY2(second.contains("<p>A line of the story that was wrapped by a typewriter &amp; goes &lt;on&gt;.</p>"),
                 qPrintable(second.left(300)));

        // A file with no details of its own is named by its file name.
        const BookMeta meta = Formats::readMeta(dir.filePath("Story - Sam Scribe.txt"));
        QCOMPARE(meta.title, QString("Story"));
        QCOMPARE(meta.authors, QStringList{"Sam Scribe"});
    }

    void markdownAndHtmlRead() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("notes.md"), "# Heading\n\nSome *emphasis* and **weight**.\n"));
        const auto markdown = Formats::open(dir.filePath("notes.md"));
        QVERIFY(markdown);
        const QString html = markdown->html(0, 0);
        QVERIFY2(html.contains("<h1>") && html.contains("Heading"), qPrintable(html));
        QVERIFY2(html.contains("<i>emphasis</i>"), qPrintable(html));
        QVERIFY2(html.contains("<b>weight</b>"), qPrintable(html));

        QVERIFY(Fixtures::write(dir.filePath("page.html"), "<html><head><title>A Page</title></head><body><p>Body</p></body></html>"));
        const auto page = Formats::open(dir.filePath("page.html"));
        QVERIFY(page);
        QCOMPARE(page->meta().title, QString("A Page"));
        QCOMPARE(page->html(0, 0), QString("<p>Body</p>"));
    }

    void pdfReadsAsPages() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::pdf(dir.filePath("paper.pdf"), "A Paper", {"First page words", "Second page words"}));
        QString error;
        const auto book = Formats::open(dir.filePath("paper.pdf"), &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->kind(), Document::Pages);
        QCOMPARE(book->count(), 2);
        QCOMPARE(book->meta().title, QString("A Paper"));
        QVERIFY(!book->meta().cover.isNull());
        QVERIFY2(book->text(1).contains("Second page words"), qPrintable(book->text(1)));
        const QImage page = book->image(0, 500);
        QCOMPARE(page.width(), 500);
        // A5 is taller than wide.
        QVERIFY(page.height() > page.width());
        QVERIFY(book->pageSize(0).height() > book->pageSize(0).width());
        QVERIFY(book->image(7, 500).isNull());

        QVERIFY(Fixtures::write(dir.filePath("fake.pdf"), "not a pdf"));
        QVERIFY(!Formats::open(dir.filePath("fake.pdf"), &error));
    }

    void comicReadsItsPagesInOrder() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::zip(dir.filePath("Saga 01.cbz"),
                              {{"page10.png", Fixtures::picture(20, 30, Qt::blue)},
                               {"page2.png", Fixtures::picture(800, 1200, Qt::green)},
                               {"Page1.png", Fixtures::picture(20, 30, Qt::red)},
                               {"__MACOSX/page1.png", Fixtures::picture(1, 1, Qt::black)},
                               {"notes.txt", "x"},
                               {"ComicInfo.xml", "<ComicInfo><Series>Saga</Series><Number>1</Number><Writer>A, B</Writer></ComicInfo>"}}));
        QString error;
        const auto comic = Formats::open(dir.filePath("Saga 01.cbz"), &error);
        QVERIFY2(comic, qPrintable(error));
        QCOMPARE(comic->kind(), Document::Pages);
        QCOMPARE(comic->count(), 3);
        // 1, 2, 10: by number, not by letter.
        QCOMPARE(comic->image(0, 0).pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(comic->image(2, 0).pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(comic->image(1, 400).size(), QSize(400, 600));
        QCOMPARE(comic->pageSize(1), QSizeF(800, 1200));
        const BookMeta meta = comic->meta();
        QCOMPARE(meta.title, QString("Saga 01"));
        QCOMPARE(meta.series, QString("Saga"));
        QCOMPARE(meta.authors, (QStringList{"A", "B"}));
        QCOMPARE(meta.cover.pixelColor(0, 0), QColor(Qt::red));
    }

    void kindleFilesSayWhoTheyAre() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("book.azw3"), Fixtures::mobi()));
        const BookMeta meta = Formats::readMeta(dir.filePath("book.azw3"));
        QCOMPARE(meta.title, QString("Kindle Fixture"));
        QCOMPARE(meta.authors, QStringList{"Kim Kindle"});
        QCOMPARE(meta.publisher, QString("Fixture Press"));
        QCOMPARE(meta.tags, (QStringList{"Testing", "Kindle"}));
        QCOMPARE(meta.language, QString("en"));
        QCOMPARE(meta.cover.size(), QSize(30, 45));

        // Read by converting, and it says so rather than failing blankly.
        QString error;
        QVERIFY(!Formats::open(dir.filePath("book.azw3"), &error));
        QVERIFY2(error.contains("converting"), qPrintable(error));
        QVERIFY(!Formats::readable("azw3"));
        QVERIFY(Formats::isBook("x.AZW3"));

        // A truncated file gives its name, not a crash.
        QVERIFY(Fixtures::write(dir.filePath("Cut - Short.mobi"), Fixtures::mobi().left(100)));
        QCOMPARE(Formats::readMeta(dir.filePath("Cut - Short.mobi")).title, QString("Cut"));
    }

    void fileNamesGiveTitleAndAuthor() {
        QCOMPARE(Formats::metaFromFileName("/x/Dune - Frank Herbert (z-lib.org).epub").title, QString("Dune"));
        QCOMPARE(Formats::metaFromFileName("/x/Dune - Frank Herbert (z-lib.org).epub").authors, QStringList{"Frank Herbert"});
        QCOMPARE(Formats::metaFromFileName("some_long_title [retail].pdf").title, QString("some long title"));
        QCOMPARE(Formats::metaFromFileName("(Untitled).pdf").title, QString("(Untitled)"));
    }

    void formatsAreKnownAndRanked() {
        QVERIFY(Formats::isBook("a.epub"));
        QVERIFY(Formats::isBook("a.CBR"));
        QVERIFY(!Formats::isBook("a.jpg"));
        QVERIFY(Formats::readable("pdf"));
        QCOMPARE(Formats::name("epub"), QString("EPUB"));
        QCOMPARE(Formats::byPreference({"mobi", "pdf", "epub"}), (QStringList{"epub", "pdf", "mobi"}));
    }

    // A book of Markdown chapters, as a folder at `folder`.
    static void markdownBook(const QString &folder) {
        QVERIFY(QDir().mkpath(folder + "/chapters/img") && QDir().mkpath(folder + "/.git"));
        QVERIFY(Fixtures::write(folder + "/README.md",
                                "---\ntitle: The Long Road\nauthors: [Ann Author, Bo Writer]\nlanguage: en\n"
                                "tags:\n  - travel\n  - essays\nseries: Roads\nseries_index: 2\n---\n"
                                "# Before you set out\n\nSee [the first day](chapters/01-first-day.md#morning).\n"));
        QVERIFY(Fixtures::write(folder + "/chapters/01-first-day.md",
                                "# The first day\n\n## Morning\n\n![A hill](img/hill.png)\n\nOn to [the second](02_second_day.md) "
                                "or [the web](https://example.com).\n"));
        QVERIFY(Fixtures::write(folder + "/chapters/02_second_day.md", "No heading here, only *words*.\n"));
        QVERIFY(Fixtures::write(folder + "/chapters/10-last.md", "Last\n====\n\nThe end.\n"));
        QVERIFY(Fixtures::write(folder + "/chapters/img/hill.png", Fixtures::picture(40, 30, Qt::darkGreen)));
        QVERIFY(Fixtures::write(folder + "/cover.png", Fixtures::picture(60, 90, Qt::red)));
        QVERIFY(Fixtures::write(folder + "/.git/config", "not part of the book"));
    }

    void aFolderOfMarkdownIsABook() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("The Long Road");
        markdownBook(folder);
        QString error;
        const auto book = Formats::open(folder, &error);
        QVERIFY2(book, qPrintable(error));
        QCOMPARE(book->kind(), Document::Flow);
        // README opens it and the rest follow by name, 2 before 10.
        QCOMPARE(book->count(), 4);
        const QList<TocEntry> toc = book->toc();
        QCOMPARE(toc.at(0).title, QString("Before you set out"));
        QCOMPARE(toc.at(1).title, QString("The first day"));
        QCOMPARE(toc.at(2).title, QString("Second day")); // from the file's name
        QCOMPARE(toc.at(3).title, QString("Last"));       // an underlined heading
        QCOMPARE(toc.at(1).level, 1);
        // Its details are the front matter's, and never shown as text.
        const BookMeta meta = book->meta();
        QCOMPARE(meta.title, QString("The Long Road"));
        QCOMPARE(meta.authors, (QStringList{"Ann Author", "Bo Writer"}));
        QCOMPARE(meta.tags, (QStringList{"travel", "essays"}));
        QCOMPARE(meta.series, QString("Roads"));
        QCOMPARE(meta.seriesIndex, 2.0);
        QCOMPARE(meta.cover.size(), QSize(60, 90));
        QVERIFY(!book->text(0).contains("authors"));
        QVERIFY(book->text(2).contains("only words"));
        // Links between files lead to chapters, pictures are found beside
        // the file that shows them, and the web stays the web.
        QVERIFY(book->html(0, 600).contains("omabook:1#morning"));
        const QString first = book->html(1, 600);
        QVERIFY(first.contains("omabook:2#"));
        QVERIFY(first.contains("https://example.com"));
        QVERIFY(first.contains("image://book/"));
        QVERIFY(!book->resource("chapters/img/hill.png").isEmpty());
        // Nothing outside the book is handed out.
        QVERIFY(book->resource("../The Long Road/README.md").isNull());
        QVERIFY(book->resource("/etc/hostname").isNull());
    }

    void theSameBookZippedReadsTheSame() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("The Long Road");
        markdownBook(folder);
        // Packed, it is one file in the format's own extension.
        QVERIFY(BookSource(folder).pack(dir.filePath("road.mdz")));
        QVERIFY(Formats::isBook(dir.filePath("road.mdz")));
        QVERIFY(Formats::readable("mdz"));
        QCOMPARE(Formats::name("mdz"), QString("Markdown book"));
        const auto folderBook = Formats::open(folder), packed = Formats::open(dir.filePath("road.mdz"));
        QVERIFY(folderBook && packed);
        QCOMPARE(packed->count(), folderBook->count());
        QCOMPARE(packed->meta().title, folderBook->meta().title);
        for (int section = 0; section < packed->count(); ++section)
            QCOMPARE(packed->text(section), folderBook->text(section));
        QVERIFY(!packed->resource("chapters/img/hill.png").isEmpty());
        // What is hidden in the folder stays out of the file.
        QVERIFY(!Archive(dir.filePath("road.mdz")).entries().join('\n').contains(".git"));
        // A plain zip of the same files is recognized by what is in it.
        QVERIFY(QFile::copy(dir.filePath("road.mdz"), dir.filePath("plain.zip")));
        QVERIFY(Formats::providerFor(dir.filePath("plain.zip")).has_value());
        QVERIFY(Formats::open(dir.filePath("plain.zip")));
        QCOMPARE(Formats::readMeta(dir.filePath("road.mdz")).title, QString("The Long Road"));
    }

    void aContentsFileSetsTheOrderAndTheNesting() {
        // mdBook's layout: book.toml, and src/SUMMARY.md listing the chapters.
        QTemporaryDir dir;
        const QString folder = dir.filePath("guide");
        QVERIFY(QDir().mkpath(folder + "/src/deep"));
        QVERIFY(Fixtures::write(folder + "/book.toml", "[book]\ntitle = \"The Guide\"\nauthors = [\"Cy Guide\"]\n"));
        QVERIFY(Fixtures::write(folder + "/src/SUMMARY.md",
                                "# Summary\n\n- [Welcome](intro.md)\n- [Basics](basics.md)\n    - [Deeper](deep/more.md)\n- [Missing](nowhere.md)\n"));
        QVERIFY(Fixtures::write(folder + "/src/intro.md", "# Something else\n\nHello.\n"));
        QVERIFY(Fixtures::write(folder + "/src/basics.md", "The basics.\n"));
        QVERIFY(Fixtures::write(folder + "/src/deep/more.md", "More.\n"));
        QVERIFY(Fixtures::write(folder + "/src/unlisted.md", "Not in the contents, so not in the book.\n"));
        const auto book = Formats::open(folder);
        QVERIFY(book);
        QCOMPARE(book->count(), 3);
        QCOMPARE(book->toc().at(0).title, QString("Welcome"));
        QCOMPARE(book->toc().at(2).title, QString("Deeper"));
        QCOMPARE(book->toc().at(1).level, 0);
        QCOMPARE(book->toc().at(2).level, 1);
        QCOMPARE(book->meta().title, QString("The Guide"));
        QCOMPARE(book->meta().authors, QStringList{"Cy Guide"});
    }

    void whatIsNotABookOfMarkdownIsLeftAlone() {
        QTemporaryDir dir;
        // A project with a README is not a book, nor is a folder of other books.
        QVERIFY(QDir().mkpath(dir.filePath("project")));
        QVERIFY(Fixtures::write(dir.filePath("project/README.md"), "# A program\n"));
        QVERIFY(Fixtures::write(dir.filePath("project/main.cpp"), "int main() {}\n"));
        QVERIFY(!Formats::providerFor(dir.filePath("project")).has_value());
        QString error;
        QVERIFY(!Formats::open(dir.filePath("project"), &error));
        QVERIFY(error.contains("doesn't know what kind of book"));
        QVERIFY(QDir().mkpath(dir.filePath("empty")));
        QVERIFY(!Formats::providerFor(dir.filePath("empty")).has_value());
        // One Markdown file on its own is read as before.
        QVERIFY(Fixtures::write(dir.filePath("note.md"), "# A note\n\nWords.\n"));
        QVERIFY(!Formats::providerFor(dir.filePath("note.md")).has_value());
        QVERIFY(Formats::open(dir.filePath("note.md")));
    }

    void aFormatIsAddedByDescribingItsBooks() {
        // The whole of a new format: which files are its own, and the book
        // in them as chapters. This one reads ".chapters" files, where a
        // line of dashes parts the chapters.
        FormatProvider provider;
        provider.id = "plugin:chapters";
        provider.name = "Chapters";
        provider.extensions = {"chapters"};
        provider.describe = [](const BookSource &source, QString *error) -> std::optional<BookDescription> {
            BookDescription book;
            book.meta.title = source.name();
            for (const QString &part : source.text(source.entries().value(0)).split("\n---\n", Qt::SkipEmptyParts))
                book.chapters.append({part.section('\n', 0, 0), "<p>" + part.section('\n', 1).toHtmlEscaped() + "</p>", {}, 0});
            if (book.chapters.isEmpty())
                *error = "It is empty.";
            return book.chapters.isEmpty() ? std::nullopt : std::optional(book);
        };
        QTemporaryDir dir;
        const QString file = dir.filePath("Two Tales.chapters");
        QVERIFY(Fixtures::write(file, "One\nThe first tale.\n---\nTwo\nThe second <b>tale</b>.\n"));
        QVERIFY(!Formats::isBook(file));

        Formats::addProvider(provider);
        QVERIFY(Formats::isBook(file));
        QVERIFY(Formats::extensions().contains("chapters"));
        QCOMPARE(Formats::name("chapters"), QString("Chapters"));
        const auto book = Formats::open(file);
        QVERIFY(book);
        QCOMPARE(book->count(), 2);
        QCOMPARE(book->toc().at(1).title, QString("Two"));
        QVERIFY(book->text(1).contains("The second <b>tale</b>."));
        QCOMPARE(Formats::readMeta(file).title, QString("Two Tales"));
        // An empty one says why, in the provider's words.
        QVERIFY(Fixtures::write(dir.filePath("none.chapters"), ""));
        QString error;
        QVERIFY(!Formats::open(dir.filePath("none.chapters"), &error));
        QCOMPARE(error, QString("It is empty."));
        // A provider can't take a format Omabook reads itself.
        provider.extensions = {"epub"};
        Formats::addProvider(provider);
        QVERIFY(Fixtures::epub(dir.filePath("one.epub")));
        QCOMPARE(Formats::open(dir.filePath("one.epub"))->meta().title, QString("The Test Book"));

        Formats::removeProvider("plugin:chapters");
        QVERIFY(!Formats::isBook(file));
    }
};

QTEST_MAIN(FormatsTests)
#include "formats_tests.moc"
