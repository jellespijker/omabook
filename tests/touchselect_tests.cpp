// Words selected by a finger (#140), the whole window on a made-up library,
// under synthetic touch and tablet events as a touchscreen laptop sends them:
// a long press selects the word under it and shows a handle at each end;
// dragging either grows or shrinks the selection a whole word at a time, both
// ways, across a line, and on through the page at its foot (a column scrolls,
// pages turn); a long press moved on without lifting grows it too, as does the
// pen held still and moved; the handles change sides in a right-to-left text,
// work on a PDF's words, and stay away while D has a finger writing on the
// page. The handles are laid out inside the page at every size, under fingers.
//
// OMABOOK_SHOTS=<folder> saves a picture of each size, to look at by eye.

#include <QGuiApplication>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QStyleHints>
#include <QTextDocument>
#include <QtTest>
#include <cmath>
#include <qpa/qwindowsysteminterface.h>

#include "bookink.h"
#include "desk.h"
#include "inksurface.h"
#include "readinghighlights.h"
#include "readingink.h"

namespace {

const QString story = "It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                      "twice, as he always did, and wrote the numbers in a book that nobody read. Mira stood at the rail and "
                      "watched the lantern on the far pier go out, one wick at a time.";
const QString arabic = "كان الوقت متأخرا في الخريف عندما عادت العبارة إلى الجزيرة. عد مدير الميناء الصناديق مرتين كما يفعل دائما.";

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &pen() {
    static QPointingDevice device{"pen", 9401, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
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

// A book of the format, its chapter `times` paragraphs of the story (or, in
// Arabic, of a few sentences of its own).
QString bookFile(const QString &folder, const QString &format, int times) {
    const QString path = folder + "/The Ferry." + format;
    bool made = false;
    if (format == "epub") {
        QString body;
        for (int i = 0; i < times; ++i)
            body += "<p>" + story + "</p>";
        made = Fixtures::chapteredEpub(path, "The Ferry", {{"The Island", body}});
    } else if (format == "txt") {
        QString body;
        for (int i = 0; i < std::max(2, times); ++i)
            body += arabic + "\n\n";
        made = Fixtures::write(path, body.toUtf8());
    } else if (format == "pdf") {
        made = Fixtures::pdf(path, "The Ferry", {"The harbour master counted the crates twice.", "Mira watched the lantern."});
    }
    return made ? path : QString();
}

// The window under fingers, a book open in it.
struct Bench {
    Desk desk;
    qint64 book = 0;
    ulong clock = 700'000;

    explicit Bench(const QString &format, const QByteArray &language = "en", const QString &layout = "scroll", int times = 2)
        : desk(language, {}, true) {
        const QString file = bookFile(desk.dir.path(), format, times);
        QVERIFY(!file.isEmpty());
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.settings->setOption("spread", "never");
        desk.settings->setOption("layout", layout);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(content());
        desk.settle();
    }

    [[nodiscard]] QQuickItem *content() const { return desk.find("contentLoader")->property("item").value<QQuickItem *>(); }
    [[nodiscard]] QString selected() const { return content()->property("selectedText").toString(); }
    [[nodiscard]] bool menuShown() const { return desk.find("readingMenu")->property("shown").toBool(); }
    [[nodiscard]] QQuickItem *handles() const { return desk.item("selectHandles"); }
    // The text a page shows: the column, or a page of the spread.
    [[nodiscard]] QQuickItem *text(int sheet = 0) const {
        if (QQuickItem *page = desk.item("page" + QString::number(sheet)))
            return page->property("text").value<QQuickItem *>();
        return desk.item("flowText");
    }
    [[nodiscard]] QTextDocument *document() const {
        auto *quick = text()->property("textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }
    [[nodiscard]] int find(const QString &words, int from = 0) const {
        return static_cast<int>(document()->toPlainText().indexOf(words, from));
    }
    // A point of the window inside a character of a page's text.
    [[nodiscard]] QPointF at(int position, int sheet = 0) const {
        QRectF rect;
        QMetaObject::invokeMethod(text(sheet), "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        const QRectF next = [&] {
            QRectF after;
            QMetaObject::invokeMethod(text(sheet), "positionToRectangle", Q_RETURN_ARG(QRectF, after), Q_ARG(int, position + 1));
            return after;
        }();
        // Halfway to the next character, whichever way the line runs.
        const double x = std::abs(next.y() - rect.y()) < 1 ? (rect.x() + next.x()) / 2 : rect.x() + 2;
        return text(sheet)->mapToScene(QPointF(x, rect.center().y()));
    }
    [[nodiscard]] int selectionStart() const { return text()->property("selectionStart").toInt(); }
    [[nodiscard]] int selectionEnd() const { return text()->property("selectionEnd").toInt(); }

    [[nodiscard]] bool shows(const QString &name) const { return desk.shows(name); }
    [[nodiscard]] QPointF middle(const QString &name) const {
        QQuickItem *item = desk.item(name);
        return item ? item->mapToScene(QPointF(item->width() / 2, item->height() / 2)) : QPointF();
    }

    void press(QPointF at) const { QTest::touchEvent(desk.window, finger()).press(0, at.toPoint(), desk.window); }
    void move(QPointF at) const { QTest::touchEvent(desk.window, finger()).move(0, at.toPoint(), desk.window); }
    void release(QPointF at) const { QTest::touchEvent(desk.window, finger()).release(0, at.toPoint(), desk.window); }
    void slide(QPointF from, QPointF to, int steps = 12) const {
        for (int i = 1; i <= steps; ++i) {
            move(from + ((to - from) * (static_cast<double>(i) / steps)));
            QTest::qWait(10);
        }
    }
    // A finger held still for longer than a long press, then lifted.
    void hold(QPointF at) const {
        press(at);
        QTest::qWait(holdMs());
        release(at);
    }
    // A handle ("selectStart", "selectEnd") taken by its knob to aim at a
    // point: where the finger goes is that far from it as the knob is from
    // the line it marks. Lifted at the end unless `stay`.
    void drag(const QString &handle, QPointF aim, bool stay = false) const {
        const QPointF knob = middle(handle + "Knob");
        const QPointF line = middle(handle + "Stem");
        const QPointF to = aim + (knob - line);
        press(knob);
        slide(knob, to);
        if (!stay)
            release(to);
    }
    // The pen at a point of the window, on its own clock.
    void penAt(QPointF at, bool down) {
        clock += 8;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    void penAlong(QPointF from, QPointF to) {
        for (int i = 1; i <= 12; ++i)
            penAt(from + ((to - from) * (i / 12.0)), true);
    }
    void penPause() const { QTest::qWait(desk.item("penGestures")->property("batchPause").toInt() + 200); }
    // Whether the knobs are the other way round from the words' start and end.
    [[nodiscard]] bool startLeftOfEnd() const { return middle("selectStartKnob").x() < middle("selectEndKnob").x(); }
};

[[nodiscard]] QString warned(const Desk &desk) { return desk.warnings.join('\n'); }

} // namespace

class TouchSelectTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // With D on a finger writes on a PDF's page while no pen is seen
    // (#134): held and moved it writes, and selects nothing. First, before
    // the pen of the tests below is seen.
    void aFingerWritingWithDOnSelectsNothing() {
        Bench bench("pdf");
        QQuickItem *page = bench.desk.item("pdfPage0");
        QTRY_VERIFY(page && page->width() > 0);
        QTest::keyClick(bench.desk.window, Qt::Key_D);
        QTRY_VERIFY(bench.desk.reader->ink()->inking());
        bench.desk.settle();
        for (int number = 0; number < 2; ++number)
            if (auto *one = qobject_cast<InkSurface *>(bench.desk.item("bookInk" + QString::number(number))))
                one->rules() = Ink::InputRules{};
        const QPointF from = page->mapToScene(QPointF(page->width() * 0.2, page->height() * 0.12));
        bench.press(from);
        QTest::qWait(holdMs());
        bench.slide(from, from + QPointF(page->width() * 0.4, page->height() * 0.05));
        bench.release(from + QPointF(page->width() * 0.4, page->height() * 0.05));
        QTest::qWait(900);
        QTRY_COMPARE(BookInk::strokes(*bench.desk.backend->library(), bench.book, 0).size(), std::size_t{1});
        QCOMPARE(bench.selected(), QString());
        QVERIFY(!bench.shows("selectHandles"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // A long press: the word, a handle at each of its ends, inside the
    // page, and its menu clear of them. A tap beside it lets it go.
    void aLongPressSelectsAWordWithHandles() {
        Bench bench("epub");
        const int lantern = bench.find("lantern");
        bench.hold(bench.at(lantern + 3));
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.selected(), QString("lantern"));
        QTRY_VERIFY(bench.shows("selectStart") && bench.shows("selectEnd"));
        QQuickItem *area = bench.desk.item("touchArea");
        const QRectF page = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()));
        for (const QString &name : {QString("selectStart"), QString("selectEnd")}) {
            QQuickItem *handle = bench.desk.item(name);
            QVERIFY2(handle->width() >= 44 && handle->height() >= 44, qPrintable(name));
            QVERIFY2(page.contains(handle->mapRectToScene(QRectF(0, 0, handle->width(), handle->height()))), qPrintable(name));
        }
        // At the word's two ends, its start's before its end's.
        QVERIFY(bench.startLeftOfEnd());
        QVERIFY(std::abs(bench.middle("selectStartStem").x() - bench.at(lantern).x()) < 8);
        QVERIFY(std::abs(bench.middle("selectEndStem").x() - bench.at(lantern + 6).x()) < 8);
        QQuickItem *menu = bench.desk.item("readingMenu");
        const QRectF knob = bench.desk.item("selectEndKnob")->mapRectToScene(QRectF(0, 0, 18, 18));
        QVERIFY(!menu->mapRectToScene(QRectF(0, 0, menu->width(), menu->height())).intersects(knob));
        // A tap closes the menu, the next one lets the words go.
        const QPointF beside = bench.at(bench.find("ferry"));
        bench.press(beside);
        bench.release(beside);
        QTRY_VERIFY(!bench.menuShown());
        QCOMPARE(bench.selected(), QString("lantern"));
        bench.press(beside);
        bench.release(beside);
        QTRY_COMPARE(bench.selected(), QString());
        QTRY_VERIFY(!bench.shows("selectHandles"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // A long press beside the words, between two paragraphs, selects
    // nothing: not the nearest word, which is on another line and may be
    // out of sight.
    void aLongPressBetweenParagraphsSelectsNothing() {
        Bench bench("epub");
        const int second = bench.find("It was", 10);
        QRectF last, first;
        QMetaObject::invokeMethod(bench.text(), "positionToRectangle", Q_RETURN_ARG(QRectF, last), Q_ARG(int, second - 2));
        QMetaObject::invokeMethod(bench.text(), "positionToRectangle", Q_RETURN_ARG(QRectF, first), Q_ARG(int, second));
        QVERIFY(first.top() - last.bottom() > 4);
        const QPointF gap = bench.text()->mapToScene(QPointF(bench.text()->width() / 2, (last.bottom() + first.top()) / 2));
        bench.hold(gap);
        QTest::qWait(200);
        QCOMPARE(bench.selected(), QString());
        QVERIFY(!bench.shows("selectHandles"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // The end's handle grows the selection on and shrinks it back, the
    // start's grows it back and shrinks it on, a whole word at a time, and
    // across a line; the menu comes back beside it.
    void theHandlesGrowAndShrinkItByWholeWords() {
        Bench bench("epub");
        bench.hold(bench.at(bench.find("lantern") + 3));
        QTRY_COMPARE(bench.selected(), QString("lantern"));
        bench.drag("selectEnd", bench.at(bench.find("pier") + 1));
        QTRY_COMPARE(bench.selected(), QString("lantern on the far pier"));
        QTRY_VERIFY(bench.menuShown());
        bench.drag("selectEnd", bench.at(bench.find("far", bench.find("lantern")) + 1));
        QTRY_COMPARE(bench.selected(), QString("lantern on the far"));
        bench.drag("selectStart", bench.at(bench.find("watched") + 2));
        QTRY_COMPARE(bench.selected(), QString("watched the lantern on the far"));
        bench.drag("selectStart", bench.at(bench.find("on the far") + 1));
        QTRY_COMPARE(bench.selected(), QString("on the far"));
        // Across lines: back to the chapter's first words, lines above.
        const int start = bench.find("It was");
        QRectF first, now;
        QMetaObject::invokeMethod(bench.text(), "positionToRectangle", Q_RETURN_ARG(QRectF, first), Q_ARG(int, start));
        QMetaObject::invokeMethod(bench.text(), "positionToRectangle", Q_RETURN_ARG(QRectF, now), Q_ARG(int, bench.selectionStart()));
        QVERIFY(now.y() > first.y() + (first.height() * 1.5));
        bench.drag("selectStart", bench.at(start + 1));
        QTRY_COMPARE(bench.selectionStart(), start);
        QVERIFY(bench.selected().startsWith("It was late in the autumn"));
        QVERIFY(bench.selected().endsWith("on the far"));
        QTRY_VERIFY(bench.menuShown());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // A long press moved on without lifting grows the selection as it goes.
    void aLongPressMovedOnGrowsIt() {
        Bench bench("epub");
        const QPointF from = bench.at(bench.find("harbour") + 2);
        const QPointF to = bench.at(bench.find("twice") + 2);
        bench.press(from);
        QTest::qWait(holdMs());
        QTRY_COMPARE(bench.selected(), QString("harbour"));
        const double still = bench.content()->property("contentY").toDouble();
        bench.slide(from, to);
        QTRY_COMPARE(bench.selected(), QString("harbour master counted the crates twice"));
        // The page held still under it.
        QCOMPARE(bench.content()->property("contentY").toDouble(), still);
        bench.release(to);
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(bench.selected(), QString("harbour master counted the crates twice"));
        QVERIFY(bench.shows("selectStart") && bench.shows("selectEnd"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // At the page's foot a column scrolls on under the finger, the
    // selection with it; pages that turn turn, and the selection goes on
    // onto the next page.
    void atThePagesFootItGoesOn() {
        Bench bench("epub", "en", "scroll", 12);
        const int word = bench.find("lantern");
        bench.hold(bench.at(word + 3));
        QTRY_COMPARE(bench.selected(), QString("lantern"));
        const double top = bench.content()->property("contentY").toDouble();
        QQuickItem *area = bench.desk.item("touchArea");
        const QPointF foot = area->mapToScene(QPointF(area->width() / 2, area->height() - 6));
        const QPointF knob = bench.middle("selectEndKnob");
        bench.press(knob);
        bench.slide(knob, foot);
        QTRY_VERIFY(bench.content()->property("contentY").toDouble() > top + 100);
        bench.release(foot);
        QTRY_VERIFY(bench.menuShown());
        QRectF end;
        QMetaObject::invokeMethod(bench.text(), "positionToRectangle", Q_RETURN_ARG(QRectF, end), Q_ARG(int, bench.selectionEnd()));
        QVERIFY(end.y() > top + area->height() - bench.text()->y());
        QCOMPARE(bench.selectionStart(), word);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }
    void atTheLastPagesFootThePageTurns() {
        Bench bench("epub", "en", "pages", 30);
        QQuickItem *spread = bench.content();
        QCOMPARE(spread->objectName(), QString("spreadReader"));
        QTRY_VERIFY(spread->property("pages").toInt() > spread->property("columns").toInt() * 2);
        const int columns = spread->property("columns").toInt();
        const auto startOf = [&](int index) {
            QVariant at;
            QMetaObject::invokeMethod(spread, "startOf", Q_RETURN_ARG(QVariant, at), Q_ARG(QVariant, index));
            return at.toInt();
        };
        // A word a line or so before the last page's end.
        const int last = columns - 1;
        const int word = bench.find(" ", startOf(columns) - 30) + 1;
        bench.hold(bench.at(word + 1, last));
        QTRY_VERIFY(!bench.selected().isEmpty());
        QTRY_VERIFY(bench.shows("selectEnd"));
        const QPointF aimOff = bench.middle("selectEndStem") - bench.middle("selectEndKnob");
        QQuickItem *page = bench.desk.item("page" + QString::number(last));
        const QPointF foot = page->mapToScene(QPointF(page->width() / 2, page->height() + 4));
        const QPointF knob = bench.middle("selectEndKnob");
        bench.press(knob);
        bench.slide(knob, foot);
        QTRY_COMPARE_WITH_TIMEOUT(spread->property("page").toInt(), columns, 3000);
        // On the new first page's words, and lifted.
        const int on = bench.find(" ", startOf(columns) + 40) + 2;
        const QPointF there = bench.at(on, 0) - aimOff;
        bench.slide(foot, there);
        bench.release(there);
        QTRY_VERIFY(bench.menuShown());
        QCOMPARE(spread->property("page").toInt(), columns);
        QCOMPARE(bench.selectionStart(), word);
        QVERIFY(bench.selectionEnd() > startOf(columns) + 40);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // In a right-to-left text the start's handle is on the right, and the
    // end's grows the selection leftwards.
    void inARightToLeftTextTheHandlesChangeSides() {
        Bench bench("txt", "ar");
        const int word = bench.find("العبارة");
        bench.hold(bench.at(word + 2));
        QTRY_COMPARE(bench.selected(), QString("العبارة"));
        QTRY_VERIFY(bench.shows("selectStart") && bench.shows("selectEnd"));
        QVERIFY(!bench.startLeftOfEnd());
        const int later = bench.find("الجزيرة");
        QVERIFY(bench.at(later + 2).x() < bench.at(word + 2).x());
        bench.drag("selectEnd", bench.at(later + 2));
        QTRY_COMPARE(bench.selected(), QString("العبارة إلى الجزيرة"));
        QVERIFY(!bench.startLeftOfEnd());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // On a PDF's page, its words (#81's boxes): a long press, its handles,
    // and the end's dragged on.
    void aPdfsWordsAreSelectedTheSameWay() {
        Bench bench("pdf");
        QQuickItem *page = bench.desk.item("pdfPage0");
        QTRY_VERIFY(page && page->width() > 0);
        const QVariantList words = bench.desk.reader->highlights()->pageWords(0, page->size());
        QVERIFY(words.size() >= 7);
        const auto point = [&](int index) {
            const QVariantMap one = words.at(index).toMap();
            return page->mapToScene(QPointF(one.value("x").toDouble() + (one.value("width").toDouble() / 2),
                                            one.value("y").toDouble() + (one.value("height").toDouble() / 2)));
        };
        bench.hold(point(1));
        QTRY_COMPARE(bench.selected(), QString("harbour"));
        QTRY_VERIFY(bench.shows("selectStart") && bench.shows("selectEnd"));
        QVERIFY(bench.startLeftOfEnd());
        bench.drag("selectEnd", point(6));
        QTRY_VERIFY(bench.selected().startsWith("harbour master counted the crates twice"));
        bench.drag("selectStart", point(0));
        QTRY_VERIFY(bench.selected().startsWith("The harbour master"));
        QTRY_VERIFY(bench.menuShown());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // The pen, D off, as a finger: held still it selects the word and shows
    // the handles, it takes a handle as a finger does, and held and then
    // moved it grows the selection to where it went.
    void thePenSelectsAsAFingerDoes() {
        Bench bench("epub");
        const QPointF lantern = bench.at(bench.find("lantern") + 3);
        bench.penAt(lantern, true);
        QTest::qWait(holdMs());
        bench.penAt(lantern, false);
        bench.penPause();
        QTRY_COMPARE(bench.selected(), QString("lantern"));
        QTRY_VERIFY(bench.shows("selectStart") && bench.shows("selectEnd"));
        const QPointF knob = bench.middle("selectEndKnob");
        const QPointF to = bench.at(bench.find("pier") + 1) + (knob - bench.middle("selectEndStem"));
        bench.penAt(knob, true);
        bench.penAlong(knob, to);
        bench.penAt(to, false);
        QTRY_COMPARE(bench.selected(), QString("lantern on the far pier"));
        QTRY_VERIFY(bench.menuShown());
        QVERIFY(bench.desk.reader->highlights()->list().isEmpty());
        // Held, then moved on.
        QMetaObject::invokeMethod(bench.content(), "clearSelection");
        QTRY_VERIFY(!bench.shows("selectHandles"));
        const QPointF from = bench.at(bench.find("harbour") + 2);
        const QPointF on = bench.at(bench.find("crates") + 2);
        bench.penAt(from, true);
        QTest::qWait(holdMs());
        bench.clock += holdMs();
        bench.penAt(from, true);
        bench.penAlong(from, on);
        bench.penAt(on, false);
        bench.penPause();
        QTRY_COMPARE(bench.selected(), QString("harbour master counted the crates"));
        QTRY_VERIFY(bench.shows("selectStart") && bench.shows("selectEnd"));
        QVERIFY(bench.desk.reader->highlights()->list().isEmpty());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // Under fingers at every size, in English and Arabic: a word selected at
    // the top of the page and one at its foot, the handles inside the page,
    // big enough to take, and the rest of the window as the survey wants it.
    void theHandlesAreInsideThePageAtEverySize_data() {
        QTest::addColumn<QByteArray>("language");
        QTest::newRow("en") << QByteArray("en");
        QTest::newRow("ar") << QByteArray("ar");
    }
    void theHandlesAreInsideThePageAtEverySize() {
        QFETCH(QByteArray, language);
        Bench bench(language == "ar" ? "txt" : "epub", language, "scroll", 30);
        QStringList problems;
        QQuickItem *handles = bench.handles();
        QQuickItem *area = bench.desk.item("touchArea");
        const auto look = [&](const QString &state) {
            bench.desk.settle();
            const QString where = QString("%1 %2x%3 %4")
                                      .arg(QString::fromLatin1(language))
                                      .arg(bench.desk.window->width())
                                      .arg(bench.desk.window->height())
                                      .arg(state);
            if (!bench.shows("selectStart") && !bench.shows("selectEnd"))
                problems << where + " — no handles";
            const QRectF page = area->mapRectToScene(QRectF(0, 0, area->width(), area->height())).adjusted(-0.5, -0.5, 0.5, 0.5);
            for (const QString &name : {QString("selectStart"), QString("selectEnd")}) {
                QQuickItem *handle = bench.desk.item(name);
                if (!handle->isVisible())
                    continue;
                if (!page.contains(handle->mapRectToScene(QRectF(0, 0, handle->width(), handle->height()))))
                    problems << where + " — " + name + " outside the page";
                QQuickItem *knob = bench.desk.item(name + "Knob");
                if (!page.contains(knob->mapRectToScene(QRectF(0, 0, knob->width(), knob->height()))))
                    problems << where + " — " + name + "'s knob outside the page";
            }
            Survey survey{bench.desk.window, true, {}};
            survey.walk();
            for (const QString &problem : std::as_const(survey.problems))
                if (!problems.contains(where + " — " + problem))
                    problems << where + " — " + problem;
            bench.desk.shot(QString("touchselect-%1-%2-%3x%4")
                                .arg(QString::fromLatin1(language), state)
                                .arg(bench.desk.window->width())
                                .arg(bench.desk.window->height()));
        };
        // A word at a point of the page, the first of those tried that has one.
        // A word of a line wholly in sight, found from the layout from that
        // far down the page upwards: whatever the fonts, a press lands on one.
        const auto pressNear = [&](double across, double down) {
            QQuickItem *text = bench.text();
            const QRectF sight = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()));
            for (int up = 0; up < 40; ++up) {
                const QPointF row = text->mapFromScene(area->mapToScene(QPointF(0, (area->height() * down) - (up * 6))));
                for (const double x : {across, 0.1, 0.3, 0.5}) {
                    int position = -1;
                    QMetaObject::invokeMethod(text, "positionAt", Q_RETURN_ARG(int, position), Q_ARG(qreal, text->width() * x),
                                              Q_ARG(qreal, row.y()));
                    QRectF line;
                    QMetaObject::invokeMethod(text, "positionToRectangle", Q_RETURN_ARG(QRectF, line), Q_ARG(int, position));
                    if (!sight.contains(text->mapRectToScene(line)))
                        continue;
                    const QPointF at = handles->mapFromScene(bench.at(position));
                    QVariant done;
                    QMetaObject::invokeMethod(handles, "pressAt", Q_RETURN_ARG(QVariant, done), Q_ARG(QVariant, at.x()),
                                              Q_ARG(QVariant, at.y()), Q_ARG(QVariant, false));
                    if (done.toBool())
                        return true;
                }
            }
            return false;
        };
        for (const QSize &size : sizes()) {
            QVERIFY(bench.desk.resize(size));
            QMetaObject::invokeMethod(bench.content(), "toStart");
            bench.desk.settle();
            QVERIFY2(pressNear(0.02, 0.06), qPrintable(QString("%1x%2").arg(size.width()).arg(size.height())));
            look("top");
            QVERIFY2(pressNear(0.6, 0.97), qPrintable(QString("%1x%2").arg(size.width()).arg(size.height())));
            look("foot");
            QMetaObject::invokeMethod(bench.content(), "clearSelection");
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }
};

QTEST_MAIN(TouchSelectTests)
#include "touchselect_tests.moc"
