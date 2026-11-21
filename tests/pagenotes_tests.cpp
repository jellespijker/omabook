// Highlights and margin notes on the pages of a PDF and a comic (#115), and
// what the reader wrote found again by its words. A PDF's page has words
// (Poppler's), and a note on them is found again by them in another edition
// as any passage is (ADR 0004); a comic's page is pictures, so a highlight or
// a note is on a part of it (a panel, PageRegion), its page its place (ADR
// 0006). In the window a part is chosen by a drag of the mouse, a finger, or
// the keys, and highlighted or noted from the menu beside it; a note is
// marked on the page and opens on a card. What was written (highlights,
// notes, bookmarks, and ink once its handwriting is read) is found by its
// words: in the reader's Find, and by "notes:" in the library's search.
// Every state is laid out at the window's sizes, in English, German, Arabic
// and under fingers. They travel between one's own computers in
// annotationsync_tests.
//
// OMABOOK_SHOTS=<folder> saves a picture of each, to look at by eye.

#include <QQuickStyle>
#include <QSignalSpy>
#include <QtTest>
#include <chrono>

#include "annotationanchors.h"
#include "annotationfinder.h"
#include "annotationwords.h"
#include "bookink.h"
#include "bookinkwords.h"
#include "booklistmodel.h"
#include "booktext.h"
#include "desk.h"
#include "highlights.h"
#include "knowledgesearch.h"
#include "marginnotes.h"
#include "readinghighlights.h"

using namespace std::chrono_literals;

namespace {

// A PDF of two pages with words, or a comic of two white pages, 800 by 1200.
QString bookFile(const QString &folder, const QString &format) {
    const QString path = folder + "/The Ferry." + format;
    const bool made = format == "pdf"
                          ? Fixtures::pdf(path, "The Ferry", {"The harbour master counted the crates twice.", "Mira watched the lantern."})
                          : Fixtures::zip(path, {{"001.png", Fixtures::picture(800, 1200, Qt::white)},
                                                 {"002.png", Fixtures::picture(800, 1200, Qt::white)}});
    return made ? path : QString();
}

// A recogniser that reads every ink as the same words, on this computer.
class Reads : public Recognition::Recognizer {
public:
    explicit Reads(QString words) : m_words(std::move(words)) {}
    [[nodiscard]] QString where() const override { return QStringLiteral("this computer"); }
    [[nodiscard]] QString model() const override { return QStringLiteral("reads-all"); }
    void recognize(const Recognition::Input &) override {
        QTimer::singleShot(0, this, [this] {
            emit recognized({.kind = Recognition::Kind::Ink,
                             .text = m_words,
                             .segments = {},
                             .language = {},
                             .model = model(),
                             .server = where(),
                             .at = 0,
                             .kept = false});
        });
    }
    void cancel() override {}

private:
    QString m_words;
};

// A line of handwriting in ink units.
Ink::Stroke line(QPointF from, QPointF to) {
    Ink::Stroke stroke{
        .id = Ink::newId(), .color = QColor("#1a1a1a"), .width = 2.4, .pressured = false, .tilted = false, .began = 1, .points = {}};
    for (int i = 0; i <= 12; ++i)
        stroke.points.push_back(
            {.at = from + ((to - from) * (i / 12.0)), .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = static_cast<quint32>(i * 8)});
    return stroke;
}

// Words typed into the window, a key at a time.
void type(QWindow *window, const QString &words) {
    for (const QChar letter : words)
        QTest::keyClick(window, letter.toLatin1());
}

// The window, a book of pages open in it.
struct Bench {
    Desk desk;
    qint64 book = 0;

    explicit Bench(const QString &format, const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false)
        : desk(language, colors, touch) {
        const QString file = bookFile(desk.dir.path(), format);
        QVERIFY(!file.isEmpty());
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.settings->setOption("layout", "scroll");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(page() && page()->width() > 0);
        desk.settle();
    }

    [[nodiscard]] Library &library() { return *desk.backend->library(); }
    [[nodiscard]] QQuickItem *page(int number = 0) const { return desk.item("pdfPage" + QString::number(number)); }
    [[nodiscard]] MarginNotes *notes() const { return desk.find("readingNotes")->property("notes").value<MarginNotes *>(); }
    [[nodiscard]] ReadingHighlights *highlights() const { return desk.reader->highlights(); }
    [[nodiscard]] QList<Annotation> made(const QString &kind) { return library().annotations(book, kind); }
    [[nodiscard]] QString mode() const { return desk.find("readingMenu")->property("mode").toString(); }
    [[nodiscard]] bool menuShown() const { return desk.find("readingMenu")->property("shown").toBool(); }
    [[nodiscard]] bool cardShown() const { return desk.find("noteCard")->property("shown").toBool(); }
    void key(int key, Qt::KeyboardModifiers modifiers = {}) const { QTest::keyClick(desk.window, static_cast<Qt::Key>(key), modifiers); }
    // A point of the window on a page, by how far across and down it.
    [[nodiscard]] QPoint on(double across, double down, int number = 0) const {
        return page(number)->mapToScene(QPointF(across * page(number)->width(), down * page(number)->height())).toPoint();
    }
    // The one of that name that is on screen.
    [[nodiscard]] QQuickItem *onScreen(const QString &name) const {
        QList<QQuickItem *> items{desk.window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (item->objectName() == name && item->isVisible() && item->width() > 0)
                return item;
            items << item->childItems();
        }
        return nullptr;
    }
    void click(const QString &name) {
        desk.settle(1);
        QQuickItem *item = onScreen(name);
        QVERIFY2(item, qPrintable(name));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
    // A drag of the mouse from one point of the window to another.
    void drag(QPoint from, QPoint to) const {
        QTest::mousePress(desk.window, Qt::LeftButton, {}, from);
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(desk.window, from + ((to - from) * step / 8));
        QTest::mouseRelease(desk.window, Qt::LeftButton, {}, to);
    }
    // A search in the reader's Find: its words, and Enter.
    void find(const QString &words) {
        QMetaObject::invokeMethod(desk.find("readerView"), "openFinder");
        QTRY_VERIFY(onScreen("findWords"));
        QObject *field = desk.find("findWords");
        field->setProperty("text", words);
        QMetaObject::invokeMethod(field, "accepted");
    }
    // A note written on the card that is open: the words, then Escape.
    void writeOnCard(const QString &words) {
        QTRY_VERIFY(cardShown());
        desk.settle();
        type(desk.window, words);
        key(Qt::Key_Escape);
    }
};

[[nodiscard]] bool near(double a, double b) { return std::abs(a - b) < 0.02; }

} // namespace

class PageNotesTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // A part of a page is kept as four short numbers, and nothing else reads as one.
    void aPartIsKeptAsItsFourNumbers() {
        const QRectF part(0.1, 0.3, 0.5, 0.4);
        QCOMPARE(PageRegion::areaText(part), QString("0.1 0.3 0.5 0.4"));
        QCOMPARE(PageRegion::areaOf(PageRegion::areaText(part)), part);
        QVERIFY(PageRegion::areaText({}).isEmpty());
        for (const char *wrong : {"", "0.1 0.2 0.3", "0.1 x 0.3 0.4", "0.1 0.2 0 0.4"})
            QVERIFY2(PageRegion::areaOf(QString(wrong)).isEmpty(), wrong);
        // Within the page across, and below its top.
        const Annotation within = Annotation::on("highlight", {.page = 2, .area = QRectF(0.8, -0.1, 0.5, 0.4)}, 1.5);
        QCOMPARE(within.area, QRectF(0.8, 0, 0.2, 0.3));
        QCOMPARE(within.place, (BookPlace{.section = 2, .fraction = 0}));
        QCOMPARE(within.to, within.place);
        QVERIFY(Annotation::on("highlight", {.page = 0, .area = QRectF(1.2, 0, 0.3, 0.3)}, 1.5).area.isEmpty());
    }

    // On a PDF's words: a note written on them, widened to whole words and
    // placed in the book's own text, stands where they are on the page and is
    // found again by them in another edition, where a highlight is too.
    void aNoteOnAPdfsWordsIsFoundAgainByThem() {
        Bench bench("pdf");
        Library &library = bench.library();
        const QString text = bench.desk.reader->document()->text(0);
        const int start = static_cast<int>(text.indexOf("master")) + 2;
        const int end = static_cast<int>(text.indexOf("crates")) + 3;
        QVERIFY(start > 1 && end > start);
        const qint64 id = bench.notes()->writeOnPage(0, start, end, "  Counts them twice  ");
        QVERIFY(id > 0);
        QCOMPARE(bench.made("margin-note").size(), 1);
        const Annotation note = bench.made("margin-note").first();
        QCOMPARE(note.quote, QString("master counted the crates"));
        QCOMPARE(note.note, QString("Counts them twice"));
        QCOMPARE(note.place.section, 0);
        QVERIFY(note.area.isEmpty());
        QCOMPARE(note.anchored, AnnotationAnchors::readableFile(library, bench.book)->hash);
        QVERIFY(bench.notes()->writeOnPage(0, start, end, "   ") == 0);
        // Where it stands: around its words on the page, its mark where they end.
        const QSizeF shown = bench.page()->size();
        const QVariantList spots = bench.notes()->pageNotes(0, shown);
        QCOMPARE(spots.size(), 1);
        const QVariantMap spot = spots.first().toMap();
        QCOMPARE(spot.value("part").toBool(), false);
        QVERIFY(spot.value("x").toDouble() > 0 && spot.value("x").toDouble() + spot.value("width").toDouble() <= shown.width());
        QVERIFY(spot.value("endX").toDouble() > spot.value("x").toDouble());
        QVERIFY(bench.notes()->pageNotes(1, shown).isEmpty());
        QTRY_VERIFY(bench.onScreen(QString("pageNoteMark_%1").arg(id)));
        // A highlight on the same page's words (#81), and another edition with words put in before them.
        QVERIFY(bench.highlights()->highlightPage(0, 0, 11, "orange") > 0);
        const BookText edition(2, [](int section) {
            return section == 0 ? QString("Early that day. The harbour master counted the crates twice.")
                                : QString("Mira watched the lantern.");
        });
        const AnnotationAnchors::Summary summary = AnnotationAnchors::anchor(library, bench.book, edition, "edition-2");
        QCOMPARE(summary.lost, 0);
        QCOMPARE(summary.moved, 2);
        const Annotation moved = bench.made("margin-note").first();
        QCOMPARE(moved.quote, QString("master counted the crates"));
        QVERIFY(moved.place.fraction > note.place.fraction);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // On a comic's panel: a highlight and a note on a part of the page, kept
    // in the page's measure, drawn at any width, listed with that part, and
    // never sought by words: in another file it is where it was.
    void aHighlightAndANoteOnAComicsPanel() {
        Bench bench("cbz");
        Library &library = bench.library();
        const QRectF panel(0.1, 0.3, 0.5, 0.4);
        const qint64 lit = bench.highlights()->highlightPart(1, panel, 1.5, "green");
        QVERIFY(lit > 0);
        const Annotation highlight = bench.made("highlight").first();
        QCOMPARE(highlight.area, panel);
        QCOMPARE(highlight.region()->page, 1);
        QCOMPARE(highlight.place.section, 1);
        QVERIFY(near(highlight.place.fraction, 0.2));
        QCOMPARE(highlight.color, QString("green"));
        QVERIFY(!highlight.hasPassage());
        QCOMPARE(bench.highlights()->highlightPart(0, QRectF(1.2, 0, 0.3, 0.3), 1.5, "green"), qint64(0));
        QCOMPARE(bench.highlights()->highlightPart(0, panel, 1.5, "violet"), qint64(0));
        // Drawn at the page's width, whatever it is.
        const QVariantMap part = bench.highlights()->pageParts(1, 400).first().toMap();
        QCOMPARE(part.value("x").toDouble(), 40.0);
        QCOMPARE(part.value("y").toDouble(), 120.0);
        QCOMPARE(part.value("width").toDouble(), 200.0);
        QCOMPARE(part.value("height").toDouble(), 160.0);
        QVERIFY(bench.highlights()->pageParts(0, 400).isEmpty());
        QCOMPARE(bench.highlights()->list().first().toMap().value("region").toMap().value("x").toDouble(), 0.1);
        // As the plugins and the trash keep it, read back the same.
        QCOMPARE(Annotation::fromJson(highlight.toJson()).area, panel);
        QCOMPARE(highlight.toJson().value("region").toObject().value("page").toInt(), 1);

        const qint64 noted = bench.notes()->writeOnPart(0, QRectF(0.2, 0.2, 0.3, 0.3), 1.5, "The ferry's light");
        QVERIFY(noted > 0);
        QCOMPARE(bench.notes()->writeOnPart(0, QRectF(0.2, 0.2, 0.3, 0.3), 1.5, "  "), qint64(0));
        const QVariantMap spot = bench.notes()->pageNotes(0, QSizeF(400, 600)).first().toMap();
        QCOMPARE(spot.value("part").toBool(), true);
        QCOMPARE(spot.value("x").toDouble(), 80.0);
        QCOMPARE(spot.value("endY").toDouble(), 200.0);
        // Another file of the book has no words to find them by: they stay, not lost.
        const AnnotationAnchors::Summary summary =
            AnnotationAnchors::anchor(library, bench.book, BookText(2, [](int) { return QString(); }), "another file");
        QCOMPARE(summary.lost, 0);
        QVERIFY(std::ranges::none_of(library.annotations(bench.book), &Annotation::lost));
        QCOMPARE(bench.made("highlight").first().area, panel);
        QTRY_VERIFY(bench.onScreen(QString("pageNoteMark_%1").arg(noted)));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // In the window: a panel chosen by a drag of the mouse is highlighted
    // from the menu beside it, and its highlight clicked has its own menu; one
    // chosen with the keys (V, the arrows, Shift to make it larger) is noted,
    // and so is one chosen by Shift+N; the note's mark opens it, and E goes
    // through the notes on the pages in sight.
    void inTheWindowAPanelIsChosenByTheMouseAndTheKeys() {
        Bench bench("cbz");
        bench.drag(bench.on(0.2, 0.1), bench.on(0.6, 0.3));
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("part"));
        bench.desk.shot("pagenotes-part-menu");
        bench.click("swatch_blue");
        QTRY_COMPARE(bench.made("highlight").size(), 1);
        const Annotation highlight = bench.made("highlight").first();
        QCOMPARE(highlight.color, QString("blue"));
        QVERIFY2(near(highlight.area.x(), 0.2) && near(highlight.area.width(), 0.4), qPrintable(PageRegion::areaText(highlight.area)));
        // Down the page in its width: a fifth of a page half again as tall as wide.
        QVERIFY2(near(highlight.area.y(), 0.15) && near(highlight.area.height(), 0.3), qPrintable(PageRegion::areaText(highlight.area)));
        QTRY_VERIFY(bench.onScreen("pagePartHighlight"));
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, bench.on(0.4, 0.2));
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("highlight"));
        bench.click("readingAction_unhighlight");
        QTRY_VERIFY(bench.made("highlight").isEmpty());

        // The keys: a part in the middle of the page, moved and made larger, then a note on it.
        bench.key(Qt::Key_V);
        QTRY_VERIFY(bench.onScreen("pagePartSelection"));
        bench.desk.shot("pagenotes-part-keys");
        const QRectF before = bench.onScreen("pagePartSelection")->mapRectToScene(QRectF(0, 0, 1, 1));
        for (int up = 0; up < 5; ++up)
            bench.key(Qt::Key_Up);
        bench.key(Qt::Key_Right);
        bench.key(Qt::Key_Down, Qt::ShiftModifier);
        QTRY_VERIFY(bench.onScreen("pagePartSelection")->mapRectToScene(QRectF(0, 0, 1, 1)).x() > before.x());
        bench.key(Qt::Key_Return);
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("part"));
        bench.click("readingAction_note");
        bench.writeOnCard("Mira waits");
        QTRY_COMPARE(bench.made("margin-note").size(), 1);
        const Annotation note = bench.made("margin-note").first();
        QCOMPARE(note.note, QString("Mira waits"));
        QVERIFY(note.region());
        QVERIFY2(near(note.area.x(), 0.3) && near(note.area.y(), 0.15) && near(note.area.height(), (0.3 * 1.5) + 0.075),
                 qPrintable(PageRegion::areaText(note.area)));

        // Shift+N with nothing chosen chooses first, and Enter writes.
        bench.key(Qt::Key_N, Qt::ShiftModifier);
        QTRY_VERIFY2(bench.onScreen("pagePartSelection"),
                     qPrintable(QString("selecting %1, focus %2, card %3")
                                    .arg(bench.desk.find("readerView")->property("selecting").toBool())
                                    .arg([&] {
                                        QStringList chain;
                                        for (QQuickItem *at = bench.desk.window->activeFocusItem(); at; at = at->parentItem())
                                            chain << QString::fromLatin1(at->metaObject()->className()) + ":" + at->objectName();
                                        return chain.join(" < ");
                                    }())
                                    .arg(bench.cardShown())));
        bench.key(Qt::Key_Up);
        bench.key(Qt::Key_Return);
        bench.writeOnCard("Gulls");
        QTRY_COMPARE(bench.made("margin-note").size(), 2);

        // Its mark opens it; E goes through them.
        const qint64 id = note.id;
        QTRY_VERIFY(bench.onScreen(QString("pageNoteMark_%1").arg(id)));
        bench.click(QString("pageNoteMark_%1").arg(id));
        QTRY_VERIFY(bench.cardShown());
        bench.desk.shot("pagenotes-part-card");
        bench.key(Qt::Key_Escape);
        QTRY_VERIFY(!bench.cardShown());
        bench.key(Qt::Key_E);
        QTRY_VERIFY(bench.cardShown());
        QVERIFY(bench.desk.find("readingNotes")->property("current").toLongLong() > 0);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // A PDF's words are noted from the keyboard as a chapter's are: Shift+N,
    // the arrows, Enter, the words.
    void aPdfsWordsAreNotedFromTheKeyboard() {
        Bench bench("pdf");
        bench.key(Qt::Key_N, Qt::ShiftModifier);
        bench.key(Qt::Key_Right, Qt::ShiftModifier);
        bench.key(Qt::Key_Return);
        bench.writeOnCard("Twice");
        QTRY_COMPARE(bench.made("margin-note").size(), 1);
        QCOMPARE(bench.made("margin-note").first().quote, QString("The harbour"));
        QTRY_VERIFY(bench.onScreen(QString("pageNoteMark_%1").arg(bench.made("margin-note").first().id)));
        bench.desk.shot("pagenotes-pdf-mark");
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // What the reader wrote is found by its words, whole or begun, in any
    // case and without its accents: by the index itself, the library's
    // search ("notes:", the shelf's and the command line's), and the reader's
    // Find, which lists them before the book's own words.
    void whatWasWrittenIsFoundByItsWords() {
        Bench bench("pdf");
        Library &library = bench.library();
        const QString text = bench.desk.reader->document()->text(0);
        const qint64 note = bench.notes()->writeOnPage(0, 0, 3, "Lighthouse keepers, again");
        QVERIFY(note > 0);
        QVERIFY(bench.highlights()->highlightPage(0, static_cast<int>(text.indexOf("harbour")),
                                                  static_cast<int>(text.indexOf("counted")) - 1, "yellow")
                > 0);
        QVERIFY(library.addAnnotation(
                    bench.book,
                    {.id = 0, .kind = "bookmark", .place = {.section = 1, .fraction = 0}, .text = "Café stop", .note = {}, .created = 0})
                > 0);

        QCOMPARE(AnnotationWords::phrases("light \"harbour  master\" -x"), (QStringList{"light", "harbour master", "-x"}));
        QVERIFY(!AnnotationWords::query({"!!", "…"}));
        const auto found = [&](const QStringList &phrases) { return AnnotationWords::in(library, bench.book, phrases).size(); };
        QCOMPARE(found({"lighthouse"}), 1);
        QCOMPARE(found({"LIGHT"}), 1);
        QCOMPARE(found({"cafe"}), 1);
        QCOMPARE(found({"harbour master"}), 1);
        QCOMPARE(found({"master harbour"}), 0);
        QCOMPARE(found({"keepers", "harbour"}), 0);
        QCOMPARE(found({"zebra"}), 0);
        QVERIFY(AnnotationWords::books(library, {"keepers"}).contains(bench.book));
        QVERIFY(AnnotationWords::books(library.root(), {"keepers"}).contains(bench.book));
        QVERIFY(AnnotationWords::books(QString(), {"keepers"}).isEmpty());

        // The search's language: notes: looks there, -notes: leaves out, a rule may not.
        const Book book = *library.book(bench.book);
        const Book::Lookups lookups{.knows = {}, .noted = AnnotationWords::noted(library)};
        QVERIFY(book.matches("notes:keepers", lookups));
        QVERIFY(book.matches("notes:\"harbour master\"", lookups));
        QVERIFY(!book.matches("notes:zebra", lookups));
        QVERIFY(book.matches("-notes:zebra", lookups));
        QVERIFY(book.matches("notes:zebra OR notes:keepers", lookups));
        QVERIFY(!book.matches("notes:keepers"));
        QVERIFY(!Book::searchProblem("notes:keepers").isEmpty());
        QVERIFY(!Book::searchProblem("rating:9 nothes:x").contains("notes:"));
        QCOMPARE(KnowledgeSearch::search(library, library.books(), "notes:lighthouse").size(), 1);
        BookListModel *shelf = bench.desk.backend->books();
        shelf->setQuery("notes:lighthouse");
        QTRY_COMPARE(shelf->count(), 1);
        shelf->setQuery("notes:zebra");
        QTRY_COMPARE(shelf->count(), 0);
        shelf->setQuery({});

        // Written anew, it is found by its new words only; removed, not at all.
        QVERIFY(bench.notes()->change(note, "Gulls over the bay"));
        QCOMPARE(found({"lighthouse"}), 0);
        QCOMPARE(found({"gulls"}), 1);

        // The reader's Find.
        AnnotationFinder finder;
        finder.setReader(&*bench.desk.reader);
        QCOMPARE(finder.onlyNotes("notes: gulls"), QString("gulls"));
        QVERIFY(finder.onlyNotes("gulls").isEmpty());
        const QVariantList gulls = finder.find("notes:gulls");
        QCOMPARE(gulls.size(), 1);
        QCOMPARE(gulls.first().toMap().value("kind").toString(), QString("margin-note"));
        QCOMPARE(gulls.first().toMap().value("title").toString(), QString("Your margin note · Page 1"));
        QVERIFY(gulls.first().toMap().value("snippet").toString().startsWith("Gulls over the bay"));
        QCOMPARE(finder.find("harbour").first().toMap().value("title").toString(), QString("Your highlight · Page 1"));
        bench.find("harbour");
        QTRY_VERIFY2(bench.desk.find("findList")->property("count").toInt() >= 2,
                     qPrintable(QString("%1 found for \"%2\"; %3")
                                    .arg(bench.desk.find("findList")->property("count").toInt())
                                    .arg(bench.desk.find("findWords")->property("text").toString(), bench.desk.warnings.join('\n'))));
        bench.desk.shot("pagenotes-find");
        QObject *list = bench.desk.find("findList");
        QVERIFY(list->property("model").toList().first().toMap().contains("kind"));
        QVERIFY(!list->property("model").toList().last().toMap().contains("kind"));
        bench.desk.closePopups();

        bench.notes()->remove(note);
        QCOMPARE(found({"gulls"}), 0);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Book ink found by the words of its handwriting: read once the pen rests,
    // only where a model is chosen to read it, never by a hosted server, and
    // kept on the page's ink, where the search finds it.
    void inkIsFoundByTheWordsReadOfIt() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::pdf(dir.filePath("ink.pdf"), "Ink", {"A page to write on."}));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("ink.pdf")).id;
        QVERIFY(BookInk::write(library, book, 0, {.added = {line({100, 100}, {600, 120})}, .removed = {}}));
        int asked = 0;
        bool hosted = true;
        const RecognitionQueue::Route route = [&](Recognition::Kind, bool hostedAsked) -> std::expected<RecognitionRoute::Ways, QString> {
            ++asked;
            hosted = hostedAsked;
            RecognitionRoute::Ways ways;
            ways.push_back(std::make_unique<Reads>(QStringLiteral("Harbour  light")));
            return ways;
        };
        BookInkWords words(route, [] { return true; }, [&library] { return &library; });
        words.setRest(10ms);
        QSignalSpy read(&words, &BookInkWords::read);
        words.changed(book, 0);
        QTRY_COMPARE(read.size(), 1);
        QCOMPARE(read.first().at(2).toString(), QString("Harbour light"));
        QCOMPARE(asked, 1);
        QCOMPARE(hosted, false);
        QCOMPARE(library.annotations(book, BookInk::kind).first().text, QString("Harbour light"));
        QCOMPARE(AnnotationWords::in(library, book, {"harbour"}).size(), 1);
        // Changes while the pen moves are read once it rests; a page with no ink, not at all.
        words.changed(book, 0);
        words.changed(book, 0);
        words.changed(book, 1);
        QTRY_COMPARE(read.size(), 2);
        QTest::qWait(60);
        QCOMPARE(asked, 2);
        QVERIFY(!BookInk::setWords(library, book, 1, "Nothing"));

        // Not set up: nothing is asked of anyone.
        BookInkWords off(route, [] { return false; }, [&library] { return &library; });
        off.setRest(10ms);
        off.changed(book, 0);
        QCOMPARE(off.pending(), 0);
        QTest::qWait(60);
        QCOMPARE(asked, 2);
        Settings settings(dir.filePath("config.json"));
        QVERIFY(!BookInkWords::setUp(settings));
        settings.setRecognition({{"ink_model", "llava"}});
        QVERIFY(BookInkWords::setUp(settings));
    }

    // Every state at the window's sizes: a panel chosen with its menu, a
    // note's card on it, a PDF's page with a note's mark, and the Find with
    // what was written, in English, German on a light theme, Arabic, and
    // German under fingers.
    void everyStateIsLaidOut_data() {
        QTest::addColumn<QByteArray>("language");
        QTest::addColumn<QByteArray>("colors");
        QTest::addColumn<bool>("touch");
        QTest::newRow("en") << QByteArray("en") << QByteArray() << false;
        QTest::newRow("de light") << QByteArray("de") << lightTheme << false;
        QTest::newRow("ar") << QByteArray("ar") << QByteArray() << false;
        QTest::newRow("de touch") << QByteArray("de") << QByteArray() << true;
    }
    void everyStateIsLaidOut() {
        QFETCH(QByteArray, language);
        QFETCH(QByteArray, colors);
        QFETCH(bool, touch);
        QStringList problems;
        const auto look = [&](Bench &bench, const QString &state) {
            bench.desk.settle();
            Survey survey{bench.desk.window, touch, {}};
            survey.walk();
            const QString where = QString("%1%2 %3x%4 %5")
                                      .arg(QString::fromLatin1(language), touch ? " touch" : "")
                                      .arg(bench.desk.window->width())
                                      .arg(bench.desk.window->height())
                                      .arg(state);
            for (const QString &problem : std::as_const(survey.problems))
                if (!problems.contains(where + " — " + problem))
                    problems << where + " — " + problem;
            bench.desk.shot(QString("pagenotes-%1%2-%3-%4x%5")
                                .arg(QString::fromLatin1(language), touch ? "-touch" : "", state)
                                .arg(bench.desk.window->width())
                                .arg(bench.desk.window->height()));
        };
        {
            Bench comic("cbz", language, colors, touch);
            QVERIFY(comic.notes()->writeOnPart(0, QRectF(0.55, 0.1, 0.35, 0.3), 1.5, "Der Leuchtturm, wieder") > 0);
            QVERIFY(comic.highlights()->highlightPart(0, QRectF(0.1, 0.5, 0.5, 0.4), 1.5, "purple") > 0);
            for (const QSize &size : dialogSizes()) {
                QVERIFY(comic.desk.resize(size));
                comic.desk.settle();
                comic.key(Qt::Key_V);
                QTRY_VERIFY(comic.onScreen("pagePartSelection"));
                look(comic, "choosing");
                comic.key(Qt::Key_Return);
                QTRY_VERIFY(comic.menuShown());
                look(comic, "part-menu");
                comic.key(Qt::Key_Escape);
                QTRY_VERIFY(!comic.menuShown());
                QObject *notes = comic.desk.find("readingNotes");
                QMetaObject::invokeMethod(notes, "go");
                QTRY_VERIFY(comic.cardShown());
                look(comic, "part-card");
                QMetaObject::invokeMethod(notes, "leave");
                QTRY_VERIFY(!comic.cardShown());
            }
        }
        {
            Bench pdf("pdf", language, colors, touch);
            QVERIFY(pdf.notes()->writeOnPage(0, 0, 11, "Twice, always") > 0);
            for (const QSize &size : dialogSizes()) {
                QVERIFY(pdf.desk.resize(size));
                pdf.desk.settle();
                look(pdf, "pdf-mark");
                pdf.find("twice");
                QTRY_VERIFY(pdf.desk.find("findList")->property("count").toInt() >= 2);
                // Measured once it has grown into place, not partway (on a busy machine it still is).
                QVERIFY(pdf.desk.waitForPopup());
                look(pdf, "find");
                pdf.desk.closePopups();
                QVERIFY2(pdf.desk.warnings.isEmpty(), qPrintable(pdf.desk.warnings.join('\n')));
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }
};

QTEST_MAIN(PageNotesTests)
#include "pagenotes_tests.moc"
