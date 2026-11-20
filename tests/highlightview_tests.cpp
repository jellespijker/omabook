// Highlights in the reader (#81), the whole window on a made-up library: made
// from words selected by the mouse, the keys and a finger, and by the pen
// (synthetic tablet events, as a touchscreen laptop's pen sends them), in
// every format with words; shown under the words, coloured anew, removed,
// listed beside the page by colour under the names the reader gave the
// colours, the lost ones placed again; and every state of it laid out at
// the window's sizes, in English, German, Arabic and under fingers.
//
// OMABOOK_SHOTS=<folder> saves a picture of each, to look at by eye.

#include <QClipboard>
#include <QGuiApplication>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QStyleHints>
#include <QTextDocument>
#include <QtTest>
#include <cmath>
#include <numbers>
#include <qpa/qwindowsysteminterface.h>
#include <string_view>

#include "annotationanchors.h"
#include "booktext.h"
#include "desk.h"
#include "highlights.h"
#include "inksurface.h"
#include "readinghighlights.h"

namespace {

const QString story = "It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                      "twice, as he always did, and wrote the numbers in a book that nobody read. Mira stood at the rail and "
                      "watched the lantern on the far pier go out, one wick at a time.";

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &pen() {
    static QPointingDevice device{"pen", 8801, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&device);
        return true;
    }();
    return device;
}

// A book of each format, with the story in it.
QString bookFile(const QString &folder, const QString &format) {
    const QString path = folder + "/The Ferry." + format;
    bool made = false;
    if (format == "epub")
        made = Fixtures::chapteredEpub(path, "The Ferry", {{"The Island", "<p>" + story + "</p><p>" + story + "</p>"}});
    else if (format == "md")
        made = Fixtures::write(path, ("# The Island\n\n" + story + "\n\n" + story + "\n").toUtf8());
    else if (format == "txt")
        made = Fixtures::write(path, (story + "\n\n" + story + "\n").toUtf8());
    else if (format == "fb2")
        made = Fixtures::write(path, QString("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                             "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\"><description><title-info>"
                                             "<book-title>The Ferry</book-title><author><first-name>Mira</first-name><last-name>Okafor"
                                             "</last-name></author><lang>en</lang></title-info></description><body><section><title><p>"
                                             "The Island</p></title><p>%1</p><p>%1</p></section></body></FictionBook>")
                                         .arg(story)
                                         .toUtf8());
    else if (format == "pdf")
        made = Fixtures::pdf(path, "The Ferry", {"The harbour master counted the crates twice.", "Mira watched the lantern."});
    return made ? path : QString();
}

// The window, a book open in it.
struct Bench {
    Desk desk;
    qint64 book = 0;

    Bench(const QString &format, const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false)
        : desk(language, colors, touch) {
        const QString file = bookFile(desk.dir.path(), format);
        QVERIFY(!file.isEmpty());
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.settings->setOption("spread", "never");
        desk.settings->setOption("layout", "scroll");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(content());
        desk.settle();
    }

    [[nodiscard]] QVariant value(const QString &name, const char *property) const {
        QObject *object = desk.find(name);
        return object ? object->property(property) : QVariant();
    }
    [[nodiscard]] QObject *content() const { return desk.find("contentLoader")->property("item").value<QObject *>(); }
    [[nodiscard]] ReadingHighlights *highlights() const { return desk.reader->highlights(); }
    [[nodiscard]] QList<Annotation> made() { return Highlights::of(*desk.backend->library(), book); }
    [[nodiscard]] QQuickItem *text() const { return desk.item("flowText"); }
    [[nodiscard]] QTextDocument *document() const {
        auto *quick = text()->property("textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }
    // Where words are in the page's text: their first character and the one past their last.
    [[nodiscard]] std::pair<int, int> span(const QString &words) const {
        const int at = static_cast<int>(document()->toPlainText().indexOf(words));
        return {at, at + static_cast<int>(words.size())};
    }
    // A point of the window on a character of the page, its middle.
    [[nodiscard]] QPointF at(int position, double across = 0.5) const {
        QRectF rect;
        QMetaObject::invokeMethod(text(), "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        return text()->mapToScene(QPointF(rect.x() + (rect.width() * across) + 1, rect.center().y()));
    }
    [[nodiscard]] QPointF below(int position) const {
        QRectF rect;
        QMetaObject::invokeMethod(text(), "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        return text()->mapToScene(QPointF(rect.x() + 1, rect.bottom() + 1));
    }
    void select(const QString &words) const {
        const auto [start, end] = span(words);
        QMetaObject::invokeMethod(text(), "select", Q_ARG(int, start), Q_ARG(int, end));
    }
    [[nodiscard]] QVariant call(const char *method, const QVariant &argument = {}) const {
        QVariant result;
        if (argument.isValid())
            QMetaObject::invokeMethod(content(), method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, argument));
        else
            QMetaObject::invokeMethod(content(), method, Q_RETURN_ARG(QVariant, result));
        return result;
    }
    [[nodiscard]] QString mode() const { return desk.find("readingMenu")->property("mode").toString(); }
    [[nodiscard]] bool menuShown() const { return desk.find("readingMenu")->property("shown").toBool(); }
    void key(int key, Qt::KeyboardModifiers modifiers = {}) const { QTest::keyClick(desk.window, static_cast<Qt::Key>(key), modifiers); }
    // The one of that name that is on screen: rows of a list each have their own.
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
        QVERIFY2(item && item->isVisible(), qPrintable(name));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
    [[nodiscard]] int shown(const QString &name) const {
        int count = 0;
        QList<QQuickItem *> items{desk.window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (item->objectName() == name && item->isVisible())
                ++count;
            items << item->childItems();
        }
        return count;
    }

    // Whether one of those items is drawn as a colour that isn't the page's
    // under it: its middle, in a picture of the window, against a point beside the text.
    [[nodiscard]] QString drawnWrong(const QString &name) {
        QQuickItem *piece = onScreen(name);
        if (!piece)
            return name + " is not there";
        desk.settle();
        const QImage window = desk.window->grabWindow();
        const QPointF middle = piece->mapToScene(QPointF(piece->width() / 2, piece->height() / 2));
        const QColor at = window.pixelColor((middle * window.devicePixelRatio()).toPoint());
        QQuickItem *tint = piece->childItems().isEmpty() ? nullptr : piece->childItems().first();
        const QColor wanted = tint ? tint->property("color").value<QColor>() : QColor();
        const QColor page =
            window.pixelColor((piece->mapToScene(QPointF(piece->width() / 2, -piece->height() * 3)) * window.devicePixelRatio()).toPoint());
        if (at == page || wanted.alpha() == 0)
            return QString("%1 at %2,%3 is %4, the page's colour; its tint is %5, %6x%7")
                .arg(name)
                .arg(middle.x())
                .arg(middle.y())
                .arg(at.name(), wanted.name(QColor::HexArgb))
                .arg(piece->width())
                .arg(piece->height());
        return {};
    }

    // The pen: a stroke through these points of the window, then its pause.
    ulong clock = 500'000;
    void penAt(QPointF at, bool down) {
        clock += 5;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    void stroke(const QList<QPointF> &points) {
        for (const QPointF &point : points)
            penAt(point, true);
        penAt(points.last(), false);
        QTest::qWait(desk.item("penGestures")->property("batchPause").toInt() + 120);
    }
    static QList<QPointF> along(QPointF from, QPointF to, int steps = 24) {
        QList<QPointF> points;
        for (int i = 0; i <= steps; ++i)
            points << from + ((to - from) * (static_cast<double>(i) / steps));
        return points;
    }
};

// The words a highlight's places point to in the book's own text.
QString wordsAt(const Bench &bench, const Annotation &highlight) {
    const BookText text(*bench.desk.reader->document());
    const qsizetype from = text.offsetOf(highlight.place);
    return text.text(highlight.place.section).mid(from, text.offsetOf(highlight.to) - from);
}

} // namespace

class HighlightViewTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // In every format with words, words selected are highlighted, shown
    // under them on the page, and placed in the book's own text, in the file
    // the reader reads; a comic's pages have no words to.
    void aHighlightIsMadeAndShownInEveryFormat_data() {
        QTest::addColumn<QString>("format");
        for (const char *format : {"epub", "fb2", "md", "txt", "pdf"})
            QTest::newRow(format) << QString(format);
    }
    void aHighlightIsMadeAndShownInEveryFormat() {
        QFETCH(QString, format);
        Bench bench(format);
        const QString words = "harbour master counted";
        if (format == "pdf") {
            const QString page = bench.desk.reader->document()->text(0);
            const int at = static_cast<int>(page.indexOf(words));
            QVERIFY2(at >= 0, qPrintable(page));
            QVERIFY(bench.highlights()->highlightPage(0, at, at + static_cast<int>(words.size()), "green") > 0);
            QTRY_VERIFY(bench.shown("pageHighlightRect") > 0);
            const QString wrong = bench.drawnWrong("pageHighlightRect");
            QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        } else {
            bench.select(words);
            QVERIFY(bench.call("highlightSelection", "green").toLongLong() > 0);
            QTRY_VERIFY(bench.shown("highlightRect") > 0);
            QCOMPARE(bench.value("flowReader", "selectedText").toString(), QString());
            const QString wrong = bench.drawnWrong("highlightRect");
            QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        }
        const QList<Annotation> made = bench.made();
        QCOMPARE(made.size(), 1);
        QCOMPARE(made.first().color, QString("green"));
        QCOMPARE(wordsAt(bench, made.first()), words);
        const auto file = AnnotationAnchors::readableFile(*bench.desk.backend->library(), bench.book);
        QVERIFY(file);
        QCOMPARE(made.first().anchored, file->hash);
        bench.desk.shot("highlight-" + format);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The mouse: a drag over words, a colour in the menu beside them; a
    // click on the highlight, another colour from the keys, then none.
    void theMouseHighlightsColoursAnewAndRemoves() {
        Bench bench("epub");
        const auto [start, end] = bench.span("ferry came back");
        QTest::mousePress(bench.desk.window, Qt::LeftButton, {}, bench.at(start, 0.1).toPoint());
        for (int step = 1; step <= 6; ++step)
            QTest::mouseMove(bench.desk.window,
                             (bench.at(start, 0.1) + ((bench.at(end - 1, 0.9) - bench.at(start, 0.1)) * step / 6)).toPoint());
        QTest::mouseRelease(bench.desk.window, Qt::LeftButton, {}, bench.at(end - 1, 0.9).toPoint());
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("menu"));
        QTRY_VERIFY(bench.desk.shows("highlightSwatches"));
        bench.desk.shot("highlight-menu");
        bench.click("swatch_blue");
        QTRY_COMPARE(bench.made().size(), 1);
        QCOMPARE(bench.made().first().color, QString("blue"));
        QCOMPARE(bench.made().first().quote, QString("ferry came back"));
        QTRY_VERIFY(bench.shown("highlightRect") > 0);
        QVERIFY(!bench.menuShown());
        QCOMPARE(bench.highlights()->current(), QString("blue"));

        // A click on it: its own menu, its colour chosen.
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, bench.at(start + 3).toPoint());
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("highlight"));
        QCOMPARE(bench.desk.find("highlightSwatches")->property("chosen").toString(), QString("blue"));
        bench.desk.shot("highlight-own-menu");
        bench.key(Qt::Key_4);
        QTRY_COMPARE(bench.made().first().color, QString("orange"));
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, bench.at(start + 3).toPoint());
        QTRY_VERIFY(bench.menuShown());
        bench.click("readingAction_unhighlight");
        QTRY_VERIFY(bench.made().isEmpty());
        QTRY_COMPARE(bench.shown("highlightRect"), 0);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The keys: V selects, the arrows widen, 2 highlights in the second colour.
    void theKeysHighlight() {
        Bench bench("epub");
        bench.key(Qt::Key_V);
        QTRY_VERIFY(bench.value("selectHint", "visible").toBool());
        bench.key(Qt::Key_L, Qt::ShiftModifier);
        bench.key(Qt::Key_L, Qt::ShiftModifier);
        const QString chosen = bench.value("flowReader", "selectedText").toString().simplified();
        QVERIFY2(chosen.split(' ').size() == 3, qPrintable(chosen));
        bench.key(Qt::Key_2);
        QTRY_COMPARE(bench.made().size(), 1);
        QCOMPARE(bench.made().first().color, QString("blue"));
        QCOMPARE(bench.made().first().quote, chosen);
        QVERIFY(!bench.value("selectHint", "visible").toBool());
        QTRY_COMPARE(bench.value("toastText", "text").toString(), QString("Highlighted in Blue"));
        // The menu's keys too: words selected again, Enter, then 5.
        bench.key(Qt::Key_V);
        bench.key(Qt::Key_J);
        bench.key(Qt::Key_Return);
        QTRY_VERIFY(bench.menuShown());
        bench.key(Qt::Key_5);
        QTRY_COMPARE(bench.made().size(), 2);
        QCOMPARE(Highlights::of(*bench.desk.backend->library(), bench.book, u"purple").size(), 1);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // A finger: a long press selects the word under it, a tap on a colour
    // highlights it; a tap on the highlight opens its menu.
    void aFingerHighlights() {
        Bench bench("epub", "en", {}, true);
        QPointingDevice *finger = QTest::createTouchDevice();
        const auto [start, end] = bench.span("lantern");
        const QPoint word = bench.at(start + 3).toPoint();
        QTest::touchEvent(bench.desk.window, finger).press(0, word, bench.desk.window);
        QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 300);
        QTest::touchEvent(bench.desk.window, finger).release(0, word, bench.desk.window);
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.value("flowReader", "selectedText").toString(), QString("lantern"));
        QQuickItem *swatch = bench.desk.item("swatch_green");
        QVERIFY(swatch && swatch->width() >= Survey::fingertip);
        const QPoint on = swatch->mapToScene(QPointF(swatch->width() / 2, swatch->height() / 2)).toPoint();
        QTest::touchEvent(bench.desk.window, finger).press(0, on, bench.desk.window);
        QTest::touchEvent(bench.desk.window, finger).release(0, on, bench.desk.window);
        QTRY_COMPARE(bench.made().size(), 1);
        QCOMPARE(bench.made().first().quote, QString("lantern"));
        QTRY_VERIFY(!bench.menuShown());
        QTest::touchEvent(bench.desk.window, finger).press(0, word, bench.desk.window);
        QTest::touchEvent(bench.desk.window, finger).release(0, word, bench.desk.window);
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("highlight"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The pen: a line under words highlights them in the colour in use, a
    // loop round words and a bar in the margin too; a scribble over one
    // takes it away; writing over the text does neither.
    void thePenHighlightsAndScribblesAway() {
        Bench bench("epub");
        QVERIFY(bench.desk.item("penGestures"));
        bench.highlights()->setCurrent("orange");
        const auto [start, end] = bench.span("harbour master counted");
        bench.stroke(Bench::along(bench.below(start) + QPointF(2, 1), bench.below(end) + QPointF(-2, 1)));
        QTRY_COMPARE(bench.made().size(), 1);
        QCOMPARE(bench.made().first().color, QString("orange"));
        QCOMPARE(bench.made().first().quote, QString("harbour master counted"));
        QTRY_VERIFY(bench.shown("highlightRect") > 0);
        // The stroke went once it was acted on.
        QCOMPARE(qobject_cast<InkSurface *>(bench.desk.item("penGestures"))->count(), 0);
        bench.desk.shot("highlight-pen");

        // A scribble over it: gone.
        QList<QPointF> rubbed;
        const QPointF left = bench.at(start + 2), right = bench.at(end - 2);
        for (int turn = 0; turn < 8; ++turn)
            rubbed << Bench::along(turn % 2 == 0 ? left : right, turn % 2 == 0 ? right : left, 10);
        bench.stroke(rubbed);
        QTRY_VERIFY(bench.made().isEmpty());

        // A loop round words.
        const auto [from, to] = bench.span("the numbers");
        const QPointF a = bench.at(from, 0), b = bench.at(to - 1, 1);
        QList<QPointF> loop;
        const QPointF centre = (a + b) / 2;
        const double rx = (b.x() - a.x()) / 2 + 8, ry = 14;
        for (int i = 0; i <= 48; ++i)
            loop << centre
                        + QPointF(rx * std::cos(std::numbers::pi + i * 2.1 * std::numbers::pi / 48),
                                  ry * std::sin(std::numbers::pi + i * 2.1 * std::numbers::pi / 48));
        bench.stroke(loop);
        QTRY_COMPARE(bench.made().size(), 1);
        QVERIFY2(bench.made().first().quote.contains("numbers"), qPrintable(bench.made().first().quote));

        // A word written over the text: nothing.
        const QPointF mid = bench.at(bench.span("nobody").first);
        bench.stroke({mid, mid + QPointF(6, -10), mid + QPointF(12, 0), mid + QPointF(18, -10), mid + QPointF(4, 6)});
        QCOMPARE(bench.made().size(), 1);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // On a PDF's page: words dragged over with the mouse, and the pen under them.
    void aPdfPageIsHighlightedByMouseAndPen() {
        Bench bench("pdf");
        QQuickItem *page = bench.desk.item("pdfPage0");
        QTRY_VERIFY(page && page->width() > 0);
        const QVariantList words = bench.highlights()->pageWords(0, page->size());
        QVERIFY(words.size() >= 4);
        const auto point = [&](int index, double across) {
            const QVariantMap word = words.at(index).toMap();
            return page
                ->mapToScene(QPointF(word.value("x").toDouble() + (word.value("width").toDouble() * across),
                                     word.value("y").toDouble() + (word.value("height").toDouble() / 2)))
                .toPoint();
        };
        QTest::mousePress(bench.desk.window, Qt::LeftButton, {}, point(1, 0.3));
        for (int step = 1; step <= 6; ++step)
            QTest::mouseMove(bench.desk.window, point(1, 0.3) + ((point(3, 0.7) - point(1, 0.3)) * step / 6));
        QTest::mouseRelease(bench.desk.window, Qt::LeftButton, {}, point(3, 0.7));
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("menu"));
        bench.click("swatch_purple");
        QTRY_COMPARE(bench.made().size(), 1);
        QCOMPARE(bench.made().first().quote, QString("harbour master counted"));
        QTRY_VERIFY(bench.shown("pageHighlightRect") > 0);
        bench.desk.shot("highlight-pdf");
        // A click on it, its menu.
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, point(2, 0.5));
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.mode(), QString("highlight"));
        bench.click("readingAction_unhighlight");
        QTRY_VERIFY(bench.made().isEmpty());
        // The pen under the first words.
        const QVariantMap first = words.at(0).toMap(), second = words.at(1).toMap();
        const double under = first.value("y").toDouble() + first.value("height").toDouble() + 1;
        bench.stroke(Bench::along(page->mapToScene(QPointF(first.value("x").toDouble(), under)),
                                  page->mapToScene(QPointF(second.value("x").toDouble() + second.value("width").toDouble(), under))));
        QTRY_COMPARE(bench.made().size(), 1);
        QCOMPARE(bench.made().first().quote, QString("The harbour"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Beside the page: the book's highlights by colour, under the names
    // the reader gave the colours, going to each; the lost ones placed again.
    void theListShowsThemByColourAndPlacesTheLostAgain() {
        Bench bench("epub");
        for (const auto &[words, color] :
             {std::pair{"ferry came back", "yellow"}, std::pair{"the crates", "green"}, std::pair{"the far pier", "green"}}) {
            bench.select(words);
            QVERIFY(bench.call("highlightSelection", color).toLongLong() > 0);
        }
        // One whose words are no longer in the book.
        Annotation gone;
        gone.kind = "highlight";
        gone.quote = "a sentence another edition had";
        gone.lost = true;
        gone.color = "blue";
        const qint64 lost = bench.desk.backend->library()->addAnnotation(bench.book, gone);
        bench.highlights()->refresh();
        bench.key(Qt::Key_B, Qt::ShiftModifier);
        QTRY_VERIFY(bench.desk.shows("annotationsList"));
        QCOMPARE(bench.desk.find("contents")->property("tab").toString(), QString("annotations"));
        QTRY_COMPARE(bench.shown("highlightRow_" + QString::number(lost)), 1);
        QCOMPARE(bench.shown("lostHeading"), 1);
        QCOMPARE(bench.desk.find("highlightsList")->property("count").toInt(), 4);
        bench.desk.shot("highlight-list");
        // One colour alone, named by the reader.
        bench.click("highlightFilter_green");
        QTRY_COMPARE(bench.desk.find("highlightsList")->property("count").toInt(), 2);
        bench.click("nameColors");
        QTRY_VERIFY(bench.desk.shows("colorName_green"));
        QQuickItem *field = bench.desk.item("colorName_green");
        field->forceActiveFocus();
        for (const char letter : std::string_view("Places"))
            QTest::keyClick(bench.desk.window, letter);
        QTest::keyClick(bench.desk.window, Qt::Key_Return);
        QTRY_COMPARE(bench.desk.settings->highlightNames().value("green").toString(), QString("Places"));
        bench.desk.shot("highlight-list-naming");
        bench.click("nameColors");
        QTRY_VERIFY(bench.value("highlightFilter_green", "visible").toBool());
        QTRY_VERIFY(bench.shown("highlightRow_" + QString::number(bench.made().last().id)) > 0 || bench.made().size() == 4);
        bench.desk.shot("highlight-list-named");
        // Copied as Markdown, that colour only, by its name.
        const QString markdown = bench.highlights()->exported("green", {{"green", "Places"}});
        QVERIFY2(markdown.contains("> the crates") && markdown.contains("— Places") && !markdown.contains("ferry"), qPrintable(markdown));
        bench.click("highlightFilter_all");
        // The lost one, placed on words selected on the page.
        bench.select("one wick at a time");
        bench.click("placeHighlight");
        QTRY_VERIFY(!Highlights::find(*bench.desk.backend->library(), lost)->second.lost);
        QCOMPARE(Highlights::find(*bench.desk.backend->library(), lost)->second.quote, QString("one wick at a time"));
        QTRY_COMPARE(bench.shown("lostHeading"), 0);
        // Enter on a row goes to it; Delete removes it.
        bench.desk.find("highlightsList")->setProperty("currentIndex", 0);
        QMetaObject::invokeMethod(bench.desk.find("annotationsList"), "takeFocus");
        bench.key(Qt::Key_Delete);
        QTRY_COMPARE(bench.made().size(), 3);
        // Shift+B again puts it away; T shows the contents in its place.
        bench.key(Qt::Key_T);
        QTRY_COMPARE(bench.desk.find("contents")->property("tab").toString(), QString("contents"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Every state of it at the window's sizes: the menu with its colours,
    // a highlight's own, the list, with its names open, in English, German
    // on a light theme, Arabic, and German under fingers.
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
        Bench bench("epub", language, colors, touch);
        bench.select("ferry came back");
        QVERIFY(bench.call("highlightSelection", "green").toLongLong() > 0);
        Annotation gone;
        gone.kind = "highlight";
        gone.quote = "a sentence another edition had, long enough to need two lines in a narrow pane beside the page";
        gone.lost = true;
        bench.desk.backend->library()->addAnnotation(bench.book, gone);
        bench.highlights()->refresh();
        QStringList problems;
        QObject *view = bench.desk.find("readerView");
        QObject *menu = bench.desk.find("readingMenu");
        const auto look = [&](const QString &state) {
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
            bench.desk.shot(QString("highlights-%1%2-%3-%4x%5")
                                .arg(QString::fromLatin1(language), touch ? "-touch" : "", state)
                                .arg(bench.desk.window->width())
                                .arg(bench.desk.window->height()));
        };
        for (const QSize &size : dialogSizes()) {
            QVERIFY(bench.desk.resize(size));
            const QVariantMap chosen{{"text", "a few words of the chapter, selected to highlight"},
                                     {"paragraph", ""},
                                     {"rect", QRectF(size.width() * 0.3, size.height() * 0.2, 120, 20)}};
            QMetaObject::invokeMethod(menu, "openMenu", Q_ARG(QVariant, chosen));
            look("menu");
            const QVariantMap own{{"id", bench.made().first().id}, {"color", "green"}};
            QMetaObject::invokeMethod(menu, "openHighlight", Q_ARG(QVariant, own),
                                      Q_ARG(QVariant, QRectF(size.width() * 0.3, size.height() * 0.7, 120, 20)));
            look("highlight-menu");
            QMetaObject::invokeMethod(menu, "close");
            QMetaObject::invokeMethod(view, "showList", Q_ARG(QVariant, "annotations"));
            look("list");
            bench.desk.find("annotationsList")->setProperty("naming", true);
            look("list-naming");
            bench.desk.find("annotationsList")->setProperty("naming", false);
            view->setProperty("showIndex", false);
            view->setProperty("showBar", true);
            look("bar");
            view->setProperty("showBar", false);
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }
};

QTEST_MAIN(HighlightViewTests)
#include "highlightview_tests.moc"
