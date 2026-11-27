// The reader under a finger and a pen (#141), the whole window on a made-up
// library, under synthetic touch and tablet events as a touchscreen laptop
// sends them: the selection menu's actions and a margin note written, kept
// and taken away by a finger; a handwritten note opened, written on and
// closed by one; the ink bar's tools; the contents, find and the AI; pages
// turned by a tap and a swipe, either way round in a right-to-left book;
// two fingers pinching the type and the pages. Then the pen, seen: a finger
// still turns, selects and taps; the pen taps what a finger does. And the
// survey of all of it under fingers at every size, right to left too.
//
// OMABOOK_SHOTS=<folder> saves a picture of each state at each size, to look
// at by eye.

#include <QClipboard>
#include <QGuiApplication>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QStyleHints>
#include <QtTest>
#include <cmath>
#include <qpa/qwindowsysteminterface.h>
#include <qpa/qwindowsysteminterface_p.h>
#include <string_view>
#include <tuple>

#include "bookink.h"
#include "desk.h"
#include "hyphenation.h"
#include "inkinput.h"
#include "inksurface.h"
#include "marginnotes.h"
#include "readinghighlights.h"
#include "readingink.h"

namespace {

const QString story = "It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                      "twice, as he always did, and Mira watched the lantern on the far pier.";
const QString arabic = "كان ذلك متأخرا في الخريف عندما عادت العبارة إلى الجزيرة. عد مدير الميناء الصناديق مرتين كما يفعل دائما.";

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &pen() {
    static QPointingDevice device{"pen", 9501, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&device);
        return true;
    }();
    return device;
}

QPointingDevice *finger() {
    static QPointingDevice *device = QTest::createTouchDevice();
    return device;
}

int holdMs() { return QGuiApplication::styleHints()->mousePressAndHoldInterval() + 300; }

// A book of the format: three chapters of the story, a long Arabic text, or
// a PDF of pages with an outline.
QString bookFile(const QString &folder, const QString &format) {
    const QString path = folder + "/The Ferry." + format;
    bool made = false;
    if (format == "epub") {
        QString body;
        for (int i = 0; i < 12; ++i)
            body += "<p>" + story + "</p>";
        made = Fixtures::chapteredEpub(path, "The Ferry", {{"The Island", body}, {"The Harbour", body}, {"The Lantern", body}});
    } else if (format == "txt") {
        QString body;
        for (int i = 0; i < 60; ++i)
            body += arabic + "\n\n";
        made = Fixtures::write(path, body.toUtf8());
    } else if (format == "pdf") {
        made = Fixtures::outlinedPdf(path, "The Ferry", 6, {{"One", 0}, {"Two", 3}});
    }
    return made ? path : QString();
}

[[nodiscard]] QPointF middleOf(const QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }

// The window under fingers, a book open in it.
struct Bench {
    Desk desk;
    qint64 book = 0;
    mutable ulong clock = 800'000;
    mutable int contact = 0;

    explicit Bench(const QString &format, const QByteArray &language = "en", const QString &layout = "scroll",
                   QSize size = QSize(1600, 900))
        : desk(language, {}, true) {
        const QString file = bookFile(desk.dir.path(), format);
        QVERIFY(!file.isEmpty());
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(size));
        desk.settings->setOption("spread", "never");
        desk.settings->setOption("layout", layout);
        desk.settings->setPages(1);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(content());
        desk.settle();
    }

    [[nodiscard]] QQuickItem *content() const { return desk.find("contentLoader")->property("item").value<QQuickItem *>(); }
    [[nodiscard]] QString selected() const { return content()->property("selectedText").toString(); }
    [[nodiscard]] bool menuShown() const { return desk.find("readingMenu")->property("shown").toBool(); }
    [[nodiscard]] QQuickItem *view() const { return desk.item("readerView"); }
    [[nodiscard]] MarginNotes *notes() const { return desk.find("readingNotes")->property("notes").value<MarginNotes *>(); }
    [[nodiscard]] QQuickItem *text() const { return desk.item("flowText"); }
    [[nodiscard]] int find(const QString &words, int from = 0) const {
        QString shown;
        QMetaObject::invokeMethod(text(), "getText", Q_RETURN_ARG(QString, shown), Q_ARG(int, 0),
                                  Q_ARG(int, text()->property("length").toInt()));
        return static_cast<int>(shown.indexOf(words, from));
    }
    // A point of the window inside a character of the text: halfway to the
    // next one, whichever way the line runs.
    [[nodiscard]] QPointF at(int position) const {
        QRectF rect, next;
        QMetaObject::invokeMethod(text(), "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        QMetaObject::invokeMethod(text(), "positionToRectangle", Q_RETURN_ARG(QRectF, next), Q_ARG(int, position + 1));
        const double x = std::abs(next.y() - rect.y()) < 1 ? (rect.x() + next.x()) / 2 : rect.x() + 2;
        return text()->mapToScene(QPointF(x, rect.center().y()));
    }
    // A point of the page, by how far across and down it.
    [[nodiscard]] QPointF on(double across, double down) const {
        QQuickItem *area = desk.item("touchArea");
        return area->mapToScene(QPointF(area->width() * across, area->height() * down));
    }

    // Where the page is, and whether the bar is up: what a tap on something
    // over the page must leave as it was.
    struct Spot {
        double percent = 0;
        double top = 0;
        int page = 0;
        bool bar = false;
    };
    [[nodiscard]] Spot spot() const {
        return {desk.reader->percent(), content()->property("contentY").toDouble(), content()->property("page").toInt(),
                view()->property("showBar").toBool()};
    }
    // What changed of it since `was`; nothing when nothing did.
    [[nodiscard]] QString movedSince(const Spot &was) const {
        QTest::qWait(400);
        const Spot now = spot();
        QStringList moved;
        if (std::abs(now.percent - was.percent) > 1e-6 || std::abs(now.top - was.top) > 0.5 || now.page != was.page)
            moved << QString("the page moved from %1 %2 %3 to %4 %5 %6")
                         .arg(was.percent)
                         .arg(was.top)
                         .arg(was.page)
                         .arg(now.percent)
                         .arg(now.top)
                         .arg(now.page);
        if (now.bar != was.bar)
            moved << QString("the bar was %1").arg(now.bar ? "shown" : "hidden");
        return moved.join("; ");
    }

    // Fingers as the window system sends them, on the clock the pen is on:
    // each point by its id, where it is and what it does.
    using Points = QList<std::tuple<int, QEventPoint::State, QPointF>>;
    void touches(const Points &points) const {
        QList<QWindowSystemInterface::TouchPoint> sent;
        const QRect screen = desk.window->screen()->geometry();
        for (const auto &[id, state, at] : points) {
            QWindowSystemInterface::TouchPoint point;
            // Each finger that comes down is a contact of its own, as a touchscreen numbers them.
            if (state == QEventPoint::State::Pressed && id == 0)
                ++contact;
            point.id = (contact * 2) + id;
            point.state = state;
            point.pressure = state == QEventPoint::State::Released ? 0.0 : 1.0;
            const QPointF global = desk.window->mapToGlobal(at);
            point.area = QRectF(0, 0, 8, 8);
            point.area.moveCenter(global);
            point.normalPosition = QPointF(global.x() / screen.width(), global.y() / screen.height());
            sent << point;
        }
        clock += 16;
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(desk.window, clock, finger(), sent);
    }
    void press(QPointF at) const { touches({{0, QEventPoint::State::Pressed, at}}); }
    void move(QPointF at) const { touches({{0, QEventPoint::State::Updated, at}}); }
    void release(QPointF at) const { touches({{0, QEventPoint::State::Released, at}}); }
    // A tap is as long as a finger's is.
    void tap(QPointF at) const {
        press(at);
        QTest::qWait(60);
        clock += 60;
        release(at);
    }
    // A finger on something on screen, which must be a fingertip; by its
    // name, or by its name inside another (two cards each have a Done).
    void tap(QQuickItem *item, const QString &name) {
        // Where it is once what just opened has been laid out.
        desk.settle();
        QVERIFY2(item && item->isVisible() && item->width() > 0, qPrintable(name + " is not on screen"));
        QVERIFY2(std::min(item->width(), item->height()) >= Survey::fingertip - 0.5, qPrintable(name + " is smaller than a fingertip"));
        tap(middleOf(item));
    }
    void tap(const QString &name) { tap(desk.item(name), name); }
    void tap(const QString &inside, const QString &name) {
        QQuickItem *outer = desk.item(inside);
        tap(outer ? outer->findChild<QQuickItem *>(name) : nullptr, inside + "/" + name);
    }
    void hold(QPointF at) const {
        press(at);
        QTest::qWait(holdMs());
        clock += holdMs();
        release(at);
    }
    void swipe(QPointF from, QPointF to) const {
        press(from);
        for (int i = 1; i <= 8; ++i) {
            move(from + ((to - from) * (i / 8.0)));
            QTest::qWait(16);
        }
        release(to);
    }
    // Two fingers, from either side of a point, apart by `spread` more at the end.
    void pinch(QPointF middle, double from, double spread) const {
        using enum QEventPoint::State;
        const QPointF half(from / 2, 0);
        touches({{0, Pressed, middle - half}});
        touches({{0, Stationary, middle - half}, {1, Pressed, middle + half}});
        QPointF now = half;
        for (int i = 1; i <= 10; ++i) {
            now = half + QPointF(spread * i / 20.0, 0);
            touches({{0, Updated, middle - now}, {1, Updated, middle + now}});
            QTest::qWait(16);
        }
        touches({{0, Released, middle - now}, {1, Released, middle + now}});
    }
    // The pen at a point of the window, on its own clock; a tap of it.
    void penAt(QPointF at, bool down) {
        clock += 8;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    void penTap(const QString &name) {
        QQuickItem *item = desk.item(name);
        QVERIFY2(item && desk.shows(name), qPrintable(name + " is not on screen"));
        const QPointF at = middleOf(item);
        penAt(at, false);
        penAt(at, true);
        penAt(at, false);
    }
    // Long after the pen, so a finger is no palm.
    void later() {
        clock += 3000;
        QTest::qWait(50);
    }
    // A row of a list whose entry says `title`.
    [[nodiscard]] QQuickItem *row(const QString &list, const QString &title) const {
        QQuickItem *view = desk.item(list);
        if (!view)
            return nullptr;
        for (QQuickItem *one : view->property("contentItem").value<QQuickItem *>()->childItems())
            if (one->property("modelData").toMap().value("title").toString() == title)
                return one;
        return nullptr;
    }
};

[[nodiscard]] QString warned(const Desk &desk) { return desk.warnings.join('\n'); }

} // namespace

class TouchReaderTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        // As on Wayland: the window system sends the pen's events and no mouse's
        // with them, so what no item takes of them Qt makes a mouse's of, which
        // is how a pen presses a button.
        QWindowSystemInterfacePrivate::TabletEvent::setPlatformSynthesizesMouse(false);
    }

    // The menu of words a finger selected: a margin note written on them,
    // its Done tapped; the note tapped again, edited, and removed by a tap.
    void aFingerWritesAMarginNoteFromTheMenu() {
        Bench bench("epub");
        bench.hold(bench.at(bench.find("lantern") + 3));
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.selected(), QString("lantern"));
        bench.tap("readingAction_note");
        QTRY_VERIFY(bench.desk.shows("marginNoteDraft"));
        QTRY_VERIFY(bench.desk.item("marginNoteDraft")->findChild<QQuickItem *>("noteField")->hasActiveFocus());
        for (const char letter : std::string_view("Bright tonight"))
            QTest::keyClick(bench.desk.window, letter);
        // Done closes the card under the finger: the tap is still the card's, wherever on the page it is.
        const Bench::Spot writing = bench.spot();
        bench.tap("marginNoteDraft", "noteDone");
        QTRY_COMPARE(bench.notes()->count(), 1);
        const QString movedByDone = bench.movedSince(writing);
        QVERIFY2(movedByDone.isEmpty(), qPrintable(movedByDone));
        const qint64 id = bench.notes()->notes().first().toMap().value("id").toLongLong();
        QCOMPARE(bench.notes()->notes().first().toMap().value("note").toString(), QString("Bright tonight"));
        const QString card = QString("marginNote_%1").arg(id);
        QTRY_VERIFY(bench.desk.shows(card));
        QVERIFY(!bench.desk.item(card)->property("editing").toBool());
        const Bench::Spot was = bench.spot();
        bench.tap(card);
        QTRY_VERIFY(bench.desk.item(card)->property("editing").toBool());
        const QString moved = bench.movedSince(was);
        QVERIFY2(moved.isEmpty(), qPrintable(moved));
        bench.tap(card, "noteRemove");
        QTRY_COMPARE(bench.notes()->count(), 0);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // Where there is no room for the margin column a note is opened by its
    // mark in the text: a finger's tap on it opens the note's card, and
    // neither turns the page nor shows the bar, wherever on the line it is.
    void aFingerOpensANoteByItsMark() {
        Bench bench("epub", "en", "scroll", QSize(480, 800));
        auto *document = bench.text()->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        // On words that end near the right edge of a line, and on words in its middle.
        const auto mark = [&](const QString &words, int after = 0) {
            const int start = bench.find(words, after);
            const qint64 id = bench.notes()->write(document, start, start + static_cast<int>(words.size()), "A note on " + words);
            return QString("noteMark_%1").arg(id);
        };
        // A word the line ends on, at the right of the page: its mark is at the edge a tap turns at.
        QString edge;
        const QQuickItem *area = bench.desk.item("touchArea");
        int from = bench.find("It was");
        for (const QString &word : story.split(' ')) {
            const int start = bench.find(word, from);
            from = start + static_cast<int>(word.size());
            const QPointF last = bench.at(from - 1), next = bench.at(from + 1);
            if (edge.isEmpty() && word.size() >= 4 && word.back().isLetter() && next.y() > last.y() + 4
                && area->mapFromScene(last).x() > area->width() * 0.75)
                edge = mark(word, start);
        }
        QVERIFY(!edge.isEmpty());
        const QString middle = mark("harbour master");
        for (const QString &name : {edge, middle}) {
            QTRY_VERIFY2(bench.desk.shows(name), qPrintable(name));
            const Bench::Spot was = bench.spot();
            bench.tap(name);
            QTRY_VERIFY2(bench.desk.find("noteCard")->property("shown").toBool(), qPrintable(name));
            const QString moved = bench.movedSince(was);
            QVERIFY2(moved.isEmpty(), qPrintable(name + ": " + moved));
            QMetaObject::invokeMethod(bench.desk.find("noteCard"), "close");
            QTRY_VERIFY(!bench.desk.find("noteCard")->property("shown").toBool());
        }
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // The menu's other actions under a finger: find the words, and copy them.
    void aFingerFindsFromTheMenu() {
        Bench bench("epub");
        bench.hold(bench.at(bench.find("harbour") + 2));
        QTRY_VERIFY(bench.menuShown());
        bench.tap("readingAction_find");
        QVERIFY(bench.desk.waitForPopup());
        QCOMPARE(bench.desk.find("findWords")->property("text").toString(), QString("harbour"));
        QTRY_VERIFY(bench.desk.item("findList")->property("count").toInt() >= 3);
        // The last chapter's place, tapped, is where the book goes.
        QQuickItem *list = bench.desk.item("findList");
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        bench.desk.settle();
        QQuickItem *last = nullptr;
        QTRY_VERIFY((last = qobject_cast<QQuickItem *>(list->property("contentItem").value<QQuickItem *>()->childItems().last())));
        for (QQuickItem *one : list->property("contentItem").value<QQuickItem *>()->childItems())
            if (one->property("index").toInt() == list->property("count").toInt() - 1)
                last = one;
        bench.tap(middleOf(last));
        QTRY_VERIFY(bench.desk.popups().isEmpty());
        QTRY_COMPARE(bench.desk.reader->section(), 2);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // A handwritten note (#90): tapped, it is edited; a finger writes on it
    // while no pen is seen, its Undo and Done are tapped.
    void aFingerOpensAndWritesAHandwrittenNote() {
        Bench bench("epub");
        QVERIFY(!Ink::penUsed());
        // Written by hand on a sentence, as the pen leaves it.
        QString shown;
        QMetaObject::invokeMethod(bench.text(), "getText", Q_RETURN_ARG(QString, shown), Q_ARG(int, 0),
                                  Q_ARG(int, bench.text()->property("length").toInt()));
        const Hyphenation::Plain plain(shown);
        const qsizetype at = plain.text().indexOf("The harbour master");
        QVERIFY(at >= 0);
        Ink::Stroke stroke{.id = Ink::newId(),
                           .color = Qt::blue,
                           .width = 2,
                           .pressured = true,
                           .tilted = false,
                           .began = 1'700'000'000'000,
                           .points = {}};
        for (int i = 0; i < 12; ++i)
            stroke.points.push_back({.at = {100.0 + (i % 2) * 6, 300.0 + i * 2}, .pressure = 0.6F, .xTilt = 0, .yTilt = 0, .ms = 0});
        const std::optional<qint64> written = bench.notes()->writeInk(shown, plain.toShown(at), plain.toShownEnd(at + 18), {stroke}, 0.5);
        QVERIFY(written);
        const qint64 id = *written;
        const QString card = QString("marginNote_%1").arg(id);
        QTRY_VERIFY(bench.desk.shows(card) && bench.desk.item(card)->property("ink").toBool());
        const Bench::Spot was = bench.spot();
        bench.tap(card);
        QTRY_VERIFY(bench.desk.item(card)->property("editing").toBool());
        const QString moved = bench.movedSince(was);
        QVERIFY2(moved.isEmpty(), qPrintable(moved));
        auto *ink = qobject_cast<InkSurface *>(bench.desk.item(card)->findChild<QQuickItem *>("noteInk"));
        QVERIFY(ink);
        const int before = ink->count();
        const QPointF middle = middleOf(ink);
        bench.swipe(middle - QPointF(30, 0), middle + QPointF(30, 6));
        QTRY_COMPARE(ink->count(), before + 1);
        bench.tap(card, "noteInkUndo");
        QTRY_COMPARE(ink->count(), before);
        bench.tap(card, "noteDone");
        QTRY_VERIFY(!bench.desk.item(card)->property("editing").toBool());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // The ink bar on a PDF (#83) under a finger: the eraser, a colour, Undo,
    // Redo, whether a finger writes, and Done; what it taps writes nothing.
    void aFingerWorksTheInkBar() {
        Bench bench("pdf", "en", "scroll", QSize(1180, 780));
        QTRY_VERIFY(bench.desk.shows("pdfPage0"));
        bench.tap(bench.on(0.5, 0.5));
        QTRY_VERIFY(bench.desk.shows("readerBar"));
        bench.tap("barInk");
        QTRY_VERIFY(bench.desk.reader->ink()->inking());
        bench.desk.settle();
        auto *surface = qobject_cast<InkSurface *>(bench.desk.item("bookInk0"));
        QVERIFY(surface);
        surface->rules() = Ink::InputRules{};
        QQuickItem *page = bench.desk.item("pdfPage0");
        const QPointF from = page->mapToScene(QPointF(page->width() * 0.2, page->height() * 0.4));
        bench.swipe(from, from + QPointF(120, 10));
        QTRY_COMPARE(BookInk::strokes(*bench.desk.backend->library(), bench.book, 0).size(), std::size_t{1});
        bench.tap("inkEraser");
        QTRY_VERIFY(bench.desk.reader->ink()->erasing());
        bench.tap("ink_red");
        QTRY_COMPARE(bench.desk.reader->ink()->color(), QString("red"));
        QVERIFY(!bench.desk.reader->ink()->erasing());
        bench.tap("inkUndo");
        QTRY_COMPARE(BookInk::strokes(*bench.desk.backend->library(), bench.book, 0).size(), std::size_t{0});
        bench.tap("inkRedo");
        QTRY_COMPARE(BookInk::strokes(*bench.desk.backend->library(), bench.book, 0).size(), std::size_t{1});
        bench.tap("inkFingers");
        QTRY_COMPARE(bench.desk.reader->ink()->fingers(), QString("on"));
        bench.tap("inkDone");
        QTRY_VERIFY(!bench.desk.reader->ink()->inking());
        QTest::qWait(700);
        QCOMPARE(BookInk::strokes(*bench.desk.backend->library(), bench.book, 0).size(), std::size_t{1});
        bench.desk.reader->ink()->setFingers("auto");
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // The button over the page stands on a ground of its own, edged in the
    // text's colour: told from a white page under a dark theme, and nothing of
    // the page shows through it (#269).
    void theMenuButtonStandsOutFromAnyPage() {
        Bench bench("pdf", "en", "scroll", QSize(700, 700));
        QVERIFY(bench.desk.pictured());
        const QQuickItem *button = bench.desk.item("menuButton");
        QVERIFY(button && button->isVisible());
        const QRect at = button->mapRectToScene(button->boundingRect()).toRect();
        const QColor ground = bench.desk.look->property("background").value<QColor>();
        const QColor ink = bench.desk.look->property("foreground").value<QColor>();
        const auto seen = [&bench](QPoint point) { return bench.desk.window->grabWindow().pixelColor(point); };
        // The page beside it is paper, as it is under it.
        QTRY_COMPARE(seen(at.topLeft() + QPoint(-6, 8)), QColor(Qt::white));
        bench.desk.shot("touch-menu-button-pdf");
        QCOMPARE(seen(at.topLeft() + QPoint(8, 8)), ground);
        QCOMPARE(seen(QPoint(at.center().x(), at.top())), ink);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // The contents (#122) under a finger: opened from the bar, a chapter
    // tapped goes there; in a narrow window it lies over the page, and a tap
    // on the page beside it puts it away.
    void aFingerGoesThroughTheContents() {
        Bench bench("epub");
        bench.tap("menuButton");
        QTRY_VERIFY(bench.desk.shows("readerBar"));
        bench.tap("barContents");
        QTRY_VERIFY(bench.desk.shows("contentsSide"));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = bench.row("contentsList", "The Lantern")));
        bench.desk.settle();
        bench.tap(middleOf(row));
        QTRY_COMPARE(bench.desk.reader->section(), 2);

        QVERIFY(bench.desk.resize(QSize(480, 800)));
        if (!bench.desk.shows("contentsSide"))
            QVERIFY(bench.desk.run("reader.contents"));
        QTRY_VERIFY(bench.desk.shows("contentsSide") && bench.desk.shows("paneScrim"));
        QTRY_VERIFY((row = bench.row("contentsList", "The Harbour")));
        bench.desk.settle();
        bench.tap(middleOf(row));
        QTRY_COMPARE(bench.desk.reader->section(), 1);
        QQuickItem *side = bench.desk.item("contentsSide");
        bench.tap(QPointF(side->width() + ((bench.desk.window->width() - side->width()) / 2.0), bench.desk.window->height() / 2.0));
        QTRY_VERIFY(!bench.desk.shows("contentsSide"));
        QCOMPARE(bench.desk.reader->section(), 1);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // The AI beside the page under a finger: opened from the bar, a button
    // tapped asks, the field tapped takes the keyboard.
    void aFingerAsksTheAi() {
        Bench bench("epub");
        bench.tap("menuButton");
        QTRY_VERIFY(bench.desk.shows("readerBar"));
        bench.tap("barAsk");
        QTRY_VERIFY(bench.desk.shows("assistantPane"));
        bench.tap("summarizeChapter");
        QTRY_VERIFY(bench.desk.item("conversation")->property("count").toInt() >= 2);
        QTRY_VERIFY(!bench.desk.assistant->property("busy").toBool());
        bench.content()->forceActiveFocus();
        bench.tap("question");
        QTRY_VERIFY(bench.desk.item("question")->hasActiveFocus());
        bench.tap("closeAssistant");
        QTRY_VERIFY(!bench.desk.shows("assistantPane"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // Pages turned by a tap at an edge and by a swipe: pages of a book that
    // turn, a PDF's, and a right-to-left book's, where forward is leftwards.
    void aFingerTurnsThePages_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QByteArray>("language");
        QTest::newRow("epub") << QString("epub") << QByteArray("en");
        QTest::newRow("pdf") << QString("pdf") << QByteArray("en");
        QTest::newRow("rtl") << QString("txt") << QByteArray("ar");
    }
    void aFingerTurnsThePages() {
        QFETCH(QString, format);
        QFETCH(QByteArray, language);
        Bench bench(format, language, "pages", QSize(1180, 780));
        const auto place = [&] {
            QQuickItem *content = bench.content();
            return content->objectName() == "spreadReader" ? content->property("page").toInt() : bench.desk.reader->section();
        };
        const auto turned = [&] {
            QTRY_VERIFY(!bench.view()->property("turningPage").toBool());
            QTRY_COMPARE(bench.view()->property("slide").toReal(), 0.0);
        };
        const bool rtl = language == "ar";
        const QPointF ahead = bench.on(rtl ? 0.08 : 0.92, 0.5), behind = bench.on(rtl ? 0.92 : 0.08, 0.5);
        const int first = place();
        bench.tap(ahead);
        turned();
        QTRY_VERIFY2(place() > first, qPrintable(QString("%1 to %2").arg(first).arg(place())));
        const int second = place();
        bench.tap(behind);
        turned();
        QTRY_COMPARE(place(), first);
        bench.swipe(ahead, behind);
        turned();
        QTRY_COMPARE(place(), second);
        bench.swipe(behind, ahead);
        turned();
        QTRY_COMPARE(place(), first);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // Two fingers spread: the type grows on a book that flows, a PDF's
    // pages grow; and they go back together again.
    void twoFingersPinch_data() {
        QTest::addColumn<QString>("format");
        QTest::newRow("epub") << QString("epub");
        QTest::newRow("pdf") << QString("pdf");
    }
    void twoFingersPinch() {
        QFETCH(QString, format);
        Bench bench(format, "en", "scroll", QSize(1180, 780));
        const auto size = [&] {
            return format == "pdf" ? bench.content()->property("zoomFactor").toDouble()
                                   : bench.desk.reader->format()->effective().value("font_size").toDouble();
        };
        const double before = size();
        bench.pinch(bench.on(0.5, 0.5), 120, 200);
        QTRY_VERIFY2(size() > before, qPrintable(QString("%1 to %2").arg(before).arg(size())));
        const double grown = size();
        bench.pinch(bench.on(0.5, 0.5), 320, -200);
        QTRY_VERIFY2(size() < grown, qPrintable(QString("%1 to %2").arg(grown).arg(size())));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // Once a pen has been seen: a finger still turns, selects and taps the
    // menu; the pen taps the bar, the menu, the contents and the ink bar's
    // tools, and writes none of them on the page.
    void withAPenSeenBothWork() {
        Bench bench("epub");
        bench.penAt(bench.on(0.5, 0.95), false);
        QVERIFY(Ink::penUsed());
        bench.later();
        const int section = bench.desk.reader->section();
        const double fraction = bench.desk.reader->fraction();
        bench.tap(bench.on(0.92, 0.5));
        QTRY_VERIFY(bench.desk.reader->fraction() > fraction || bench.desk.reader->section() > section);
        QTRY_VERIFY(!bench.view()->property("turningPage").toBool());
        QMetaObject::invokeMethod(bench.content(), "toStart");
        bench.desk.settle();
        bench.hold(bench.at(bench.find("lantern") + 3));
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.selected(), QString("lantern"));
        bench.penTap("readingAction_copy");
        QTRY_VERIFY(!bench.menuShown());
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QString("lantern"));
        // The bar and the contents, by the pen.
        bench.penTap("menuButton");
        QTRY_VERIFY(bench.desk.shows("readerBar"));
        bench.penTap("barContents");
        QTRY_VERIFY(bench.desk.shows("contentsSide"));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = bench.row("contentsList", "The Harbour")));
        const QPointF on = middleOf(row);
        bench.penAt(on, false);
        bench.penAt(on, true);
        bench.penAt(on, false);
        QTRY_COMPARE(bench.desk.reader->section(), 1);
        QVERIFY(bench.desk.reader->highlights()->list().isEmpty());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }
    void withAPenSeenTheInkBarTakesBoth() {
        Bench bench("pdf", "en", "scroll", QSize(1180, 780));
        QTRY_VERIFY(bench.desk.shows("pdfPage0"));
        QTest::keyClick(bench.desk.window, Qt::Key_D);
        QTRY_VERIFY(bench.desk.reader->ink()->inking());
        bench.desk.settle();
        bench.penTap("inkEraser");
        QTRY_VERIFY(bench.desk.reader->ink()->erasing());
        bench.penTap("ink_blue");
        QTRY_COMPARE(bench.desk.reader->ink()->color(), QString("blue"));
        // The bar over the page is the pen's to press, not to write on.
        bench.tap("menuButton");
        QTRY_VERIFY(bench.desk.shows("readerBar"));
        bench.penTap("barContents");
        QTRY_VERIFY(bench.desk.shows("contentsSide"));
        QVERIFY(bench.desk.run("reader.contents"));
        QTRY_VERIFY(!bench.desk.shows("contentsSide"));
        bench.later();
        bench.tap("inkEraser");
        QTRY_VERIFY(bench.desk.reader->ink()->erasing());
        bench.tap("ink_green");
        QTRY_COMPARE(bench.desk.reader->ink()->color(), QString("green"));
        QTest::qWait(700);
        QCOMPARE(BookInk::strokes(*bench.desk.backend->library(), bench.book, 0).size(), std::size_t{0});
        bench.penTap("inkDone");
        QTRY_VERIFY(!bench.desk.reader->ink()->inking());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // Under fingers at every size, in English and right to left: the
    // selection's menu and handles, the contents, find, the AI, a margin
    // note being written, and a PDF's ink bar, as the survey wants them.
    void theReaderUnderFingersAtEverySize_data() {
        QTest::addColumn<QByteArray>("language");
        QTest::newRow("en") << QByteArray("en");
        QTest::newRow("ar") << QByteArray("ar");
    }
    void theReaderUnderFingersAtEverySize() {
        QFETCH(QByteArray, language);
        QStringList problems;
        const auto look = [&](Bench &bench, const QString &state) {
            bench.desk.settle();
            const QString where = QString("%1 %2x%3 %4")
                                      .arg(QString::fromLatin1(language))
                                      .arg(bench.desk.window->width())
                                      .arg(bench.desk.window->height())
                                      .arg(state);
            Survey survey{bench.desk.window, true, {}};
            survey.walk();
            for (const QString &problem : std::as_const(survey.problems))
                if (!problems.contains(where + " — " + problem))
                    problems << where + " — " + problem;
            bench.desk.shot(QString("touchreader-%1-%2-%3x%4")
                                .arg(QString::fromLatin1(language), state)
                                .arg(bench.desk.window->width())
                                .arg(bench.desk.window->height()));
        };
        {
            Bench bench(language == "ar" ? "txt" : "epub", language);
            for (const QSize &size : sizes()) {
                QVERIFY(bench.desk.resize(size));
                QMetaObject::invokeMethod(bench.content(), "toStart");
                bench.desk.settle();
                // A word near the top of the page, found from the layout: whatever the fonts, a press lands on one.
                QQuickItem *text = bench.text();
                QQuickItem *handles = bench.desk.item("selectHandles");
                bool pressed = false;
                for (int row = 0; row < 12 && !pressed; ++row)
                    for (const double across : {0.5, 0.3, 0.7}) {
                        const QPointF there = text->mapFromScene(bench.on(across, 0.1 + (row * 0.03)));
                        int position = -1;
                        QMetaObject::invokeMethod(text, "positionAt", Q_RETURN_ARG(int, position), Q_ARG(qreal, there.x()),
                                                  Q_ARG(qreal, there.y()));
                        const QPointF at = handles->mapFromScene(bench.at(position));
                        QVariant done;
                        QMetaObject::invokeMethod(handles, "pressAt", Q_RETURN_ARG(QVariant, done), Q_ARG(QVariant, at.x()),
                                                  Q_ARG(QVariant, at.y()), Q_ARG(QVariant, false));
                        if ((pressed = done.toBool()))
                            break;
                    }
                QVERIFY2(pressed, qPrintable(QString("%1x%2").arg(size.width()).arg(size.height())));
                QTRY_VERIFY(bench.menuShown());
                look(bench, "menu");
                QMetaObject::invokeMethod(bench.content(), "clearSelection");
                bench.desk.find("readingMenu")->setProperty("shown", false);
                for (const char *pane : {"reader.contents", "reader.ask"}) {
                    QVERIFY(bench.desk.run(pane));
                    look(bench, QString::fromLatin1(pane).mid(7));
                    QVERIFY(bench.desk.run("reader.back"));
                    QTRY_VERIFY(bench.desk.reader->isOpen());
                    bench.desk.settle();
                }
                QVERIFY(bench.desk.run("reader.find"));
                QVERIFY(bench.desk.waitForPopup());
                look(bench, "find");
                QVERIFY(bench.desk.closePopups());
            }
        }
        {
            Bench bench("pdf", language);
            QTest::keyClick(bench.desk.window, Qt::Key_D);
            QTRY_VERIFY(bench.desk.reader->ink()->inking());
            for (const QSize &size : sizes()) {
                QVERIFY(bench.desk.resize(size));
                look(bench, "ink");
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }
};

QTEST_MAIN(TouchReaderTests)
#include "touchreader_tests.moc"
