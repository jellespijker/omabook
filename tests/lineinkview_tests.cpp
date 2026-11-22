// Free ink on a flowing book (#152), the whole window on a made-up library:
// with D on, the pen writes in the margins of an EPUB and between its lines;
// each stroke is kept on the line beside it and drawn beside that line again
// when the type grows, the window narrows and the book turns to pages that
// turn, in a margin too narrow for it smaller (#157). Undone and done again a
// pause of the pen at a time, listed by chapter and erased from the list; the
// highlighter and the lasso; a finger writing as on a PDF (#134); the inks in
// the theme's colours; another computer's ink coming onto the page; and every
// state laid out at the window's sizes, in English, German, Arabic and under
// fingers. Pen and fingers are synthetic tablet and touch events.
//
// OMABOOK_SHOTS=<folder> saves a picture of each, to look at by eye.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QTextDocument>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>

#include "desk.h"
#include "inksurface.h"
#include "lineink.h"
#include "readingink.h"
#include "readinglineink.h"
#include "textgeometry.h"

namespace {

using State = QEventPoint::State;

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &pen() {
    static QPointingDevice device{"pen", 9501, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&device);
        return true;
    }();
    return device;
}

QPointingDevice *fingers() {
    static QPointingDevice *device = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen,
                                                              QInputDevice::Capability::Position | QInputDevice::Capability::Area);
    return device;
}

const QString story = "The harbour master counted the crates twice before the ferry left, and twice more when it came back empty. "
                      "Mira watched the lantern swing over the black water while the gulls slept on the roof of the ticket office. "
                      "Nobody spoke of the storm, though everyone had seen the sky go green at noon, and the old captain had tied "
                      "his boat to the pier with three ropes instead of one.";

// The window, an EPUB open in it, scrolling.
struct Bench {
    Desk desk;
    qint64 book = 0;
    ulong clock = 900'000;

    explicit Bench(const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false) : desk(language, colors, touch) {
        const QString file = desk.dir.filePath("The Ferry.epub");
        QVERIFY(Fixtures::chapteredEpub(
            file, "The Ferry", {{"The Crossing", "<p>" + story + "</p><p>" + story + "</p>"}, {"The Return", "<p>" + story + "</p>"}}));
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.settings->setOption("layout", "scroll");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(text() && text()->width() > 0 && surface());
        desk.settle();
    }

    [[nodiscard]] QQuickItem *text() const { return desk.item("flowText"); }
    [[nodiscard]] InkSurface *surface() const { return qobject_cast<InkSurface *>(desk.item("lineInk")); }
    [[nodiscard]] ReadingInk *ink() const { return desk.reader->ink(); }
    [[nodiscard]] ReadingLineInk *lineInk() const { return desk.reader->lineInk(); }
    [[nodiscard]] QList<LineInk::Line> kept() { return LineInk::lines(*desk.backend->library(), book); }
    [[nodiscard]] const QTextDocument *document() const {
        return text()->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    }
    void key(int key, Qt::KeyboardModifiers modifiers = {}) const { QTest::keyClick(desk.window, static_cast<Qt::Key>(key), modifiers); }
    // Where a word of the chapter is in the window: its first rectangle; in the
    // second paragraph by default, below the writing bar however it wraps.
    [[nodiscard]] QRectF word(const QString &words, int occurrence = 1) const {
        const QString shown = document()->toPlainText();
        qsizetype at = -1;
        for (int i = 0; i <= occurrence; ++i)
            at = shown.indexOf(words, at + 1);
        const QList<QRectF> rects = TextGeometry::rects(*document(), static_cast<int>(at), static_cast<int>(at + words.size()));
        return rects.isEmpty() ? QRectF() : text()->mapRectToScene(rects.first());
    }
    [[nodiscard]] QRectF columnInScene() const { return text()->mapRectToScene(QRectF(0, 0, text()->width(), text()->height())); }

    void penAt(QPointF at, bool down) {
        clock += 8;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    // A stroke through these points of the window, and the pause after it, when the batch goes.
    void stroke(QPointF from, QPointF to) {
        for (int i = 0; i <= 16; ++i)
            penAt(from + ((to - from) * (i / 16.0)), true);
        penAt(to, false);
        QTest::qWait(surface()->batchPause() + 150);
    }
    [[nodiscard]] QWindowSystemInterface::TouchPoint finger(State state, QPointF at) const {
        QWindowSystemInterface::TouchPoint point;
        point.state = state;
        point.pressure = state == State::Released ? 0.0 : 1.0;
        const QPointF global = desk.window->mapToGlobal(at);
        point.area = QRectF(0, 0, 9, 9);
        point.area.moveCenter(global);
        const QRect screen = desk.window->screen()->geometry();
        point.normalPosition = QPointF(global.x() / screen.width(), global.y() / screen.height());
        return point;
    }
    void swipe(QPointF from, QPointF to) {
        const auto touch = [this](const QWindowSystemInterface::TouchPoint &point) {
            clock += 12;
            QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(desk.window, clock, fingers(), {point});
        };
        touch(finger(State::Pressed, from));
        for (int i = 1; i <= 12; ++i)
            touch(finger(State::Updated, from + ((to - from) * (i / 12.0))));
        touch(finger(State::Released, to));
        QTest::qWait(surface()->batchPause() + 150);
    }
    // Ink in the left margin beside the line of a word, and under the word: a margin stroke and an underline.
    void writeBeside(const QString &words) {
        const QRectF at = word(words);
        const QRectF column = columnInScene();
        stroke(QPointF(column.left() - 34, at.center().y() - 5), QPointF(column.left() - 12, at.center().y() + 5));
        stroke(QPointF(at.left() + 1, at.bottom() - 1), QPointF(at.right() - 1, at.bottom() - 1));
    }
    // The stroke of a kind kept beside the words, as the surface draws it now, in the window.
    [[nodiscard]] QRectF drawn(const QString &kind) {
        for (const LineInk::Line &line : kept())
            if (line.kind == kind && !line.strokes.empty())
                if (const Ink::Stroke *shown = surface()->sheet().find(line.strokes.front().id))
                    return surface()->mapRectToScene(Ink::bounds(*shown));
        return {};
    }
    // Whether the margin's stroke is beside the word's line, left of the column, and the underline under the word.
    [[nodiscard]] QString notBeside(const QString &words, const QString &state) {
        const QRectF at = word(words);
        const QRectF margin = drawn("margin-ink");
        const QRectF under = drawn("line-ink");
        const QRectF column = columnInScene();
        if (margin.isNull() || under.isNull())
            return state + ": not drawn";
        if (margin.right() > column.left() + 0.5 || margin.center().y() < at.top() || margin.center().y() > at.bottom())
            return QString("%1: the margin's ink at %2,%3 is not beside the line at %4..%5 left of %6")
                .arg(state)
                .arg(margin.center().x())
                .arg(margin.center().y())
                .arg(at.top())
                .arg(at.bottom())
                .arg(column.left());
        if (std::abs(under.left() - at.left()) > 4 || under.center().y() < at.center().y() || under.top() > at.bottom() + 4)
            return QString("%1: the underline at %2,%3 is not under the word at %4,%5")
                .arg(state)
                .arg(under.left())
                .arg(under.top())
                .arg(at.left())
                .arg(at.bottom());
        return {};
    }
};

} // namespace

class LineInkViewTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // D, and the pen writes in the margin and between the lines: each stroke
    // kept on its line, the margin's on the whole line, the underline on its
    // word; it highlights nothing. Ctrl+Z undoes a pause of the pen at a time
    // and Ctrl+Shift+Z does it again. D again, and the pen is the gestures' once more.
    void thePenWritesInTheMarginAndBetweenTheLines() {
        Bench bench;
        bench.key(Qt::Key_D);
        QTRY_VERIFY(bench.ink()->inking());
        QTRY_VERIFY(bench.desk.shows("inkBar"));
        QVERIFY(bench.surface()->isEnabled() && !bench.surface()->readOnly());
        bench.writeBeside("lantern");
        QTRY_COMPARE(bench.kept().size(), 2);
        const QList<LineInk::Line> lines = bench.kept();
        const auto margin = std::ranges::find(lines, QString("margin-ink"), &LineInk::Line::kind);
        const auto under = std::ranges::find(lines, QString("line-ink"), &LineInk::Line::kind);
        QVERIFY(margin != lines.cend() && under != lines.cend());
        QVERIFY2(margin->passage.quote.contains("lantern"), qPrintable(margin->passage.quote));
        QCOMPARE(under->passage.quote, QString("lantern"));
        QCOMPARE(margin->passage.from.section, 0);
        QVERIFY(noHighlights(bench));
        const QString off = bench.notBeside("lantern", "as drawn");
        QVERIFY2(off.isEmpty(), qPrintable(off));
        bench.desk.shot("lineink-written");

        bench.key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(bench.kept().size(), 1);
        QCOMPARE(bench.surface()->count(), 1);
        bench.key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(bench.kept().isEmpty());
        QCOMPARE(bench.surface()->count(), 0);
        bench.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        bench.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(bench.kept().size(), 2);
        QCOMPARE(bench.surface()->count(), 2);

        bench.key(Qt::Key_D);
        QTRY_VERIFY(!bench.ink()->inking());
        QVERIFY(!bench.surface()->isEnabled());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The ink stays beside its line as the type grows, the window narrows, and
    // the book is read as pages that turn; at the line's height each time.
    void inkFollowsItsLineThroughReflow() {
        Bench bench;
        bench.ink()->setInking(true);
        bench.desk.settle();
        bench.writeBeside("gulls");
        QTRY_COMPARE(bench.kept().size(), 2);
        const double drawnAt = bench.drawn("margin-ink").height() / bench.word("gulls").height();
        const auto check = [&](const QString &state) {
            QTRY_VERIFY2(bench.notBeside("gulls", state).isEmpty(), qPrintable(bench.notBeside("gulls", state)));
            // As high beside its line as it was, or, in a margin narrower than that (#157), as wide as the margin.
            const QRectF margin = bench.drawn("margin-ink");
            const double now = margin.height() / bench.word("gulls").height();
            const double edge = bench.surface()->mapToScene(QPointF(0, 0)).x();
            QVERIFY2(std::abs(now - drawnAt) < 0.05 || (now < drawnAt && std::abs(margin.left() - edge) < 1.5),
                     qPrintable(QString("%1: %2 of a line high, drawn %3, from %4 in a margin from %5")
                                    .arg(state)
                                    .arg(now)
                                    .arg(drawnAt)
                                    .arg(margin.left())
                                    .arg(edge)));
            bench.desk.shot("lineink-" + state);
        };
        check("as-drawn");
        bench.desk.settings->setOption("font_size", 26);
        bench.desk.settle();
        check("larger-type");
        QVERIFY(bench.desk.resize(QSize(820, 780)));
        check("narrower");
        bench.desk.settings->setOption("font_size", 18);
        bench.desk.settings->setOption("layout", "pages");
        QTRY_VERIFY(bench.desk.item("spreadReader"));
        QTRY_VERIFY(bench.surface() && bench.surface()->count() == 2);
        check("pages");
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Type so large the margin is narrower than the ink (#157): it is drawn
    // smaller, within the page, and at its size again once the window grows.
    void inkInATooNarrowMarginIsDrawnSmallerWithinIt() {
        Bench bench;
        bench.ink()->setInking(true);
        bench.desk.settle();
        const QRectF at = bench.word("lantern");
        const QRectF column = bench.columnInScene();
        bench.stroke(QPointF(column.left() - 50, at.center().y()), QPointF(column.left() - 8, at.center().y()));
        QTRY_COMPARE(bench.kept().size(), 1);
        QVERIFY(bench.desk.resize(QSize(560, 700)));
        bench.desk.settings->setOption("font_size", 34);
        bench.desk.settle();
        QTRY_VERIFY(!bench.drawn("margin-ink").isNull());
        const QRectF small = bench.drawn("margin-ink");
        const QRectF view = bench.desk.item("flowReader")->mapRectToScene(QRectF(QPointF(), bench.desk.item("flowReader")->size()));
        QVERIFY2(small.left() >= view.left() - 0.5 && small.right() <= bench.columnInScene().left() + 0.5,
                 qPrintable(
                     QString("%1..%2 in %3..%4").arg(small.left()).arg(small.right()).arg(view.left()).arg(bench.columnInScene().left())));
        bench.desk.shot("lineink-large-type");
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The highlighter draws see-through and as wide all along; the lasso takes
    // a stroke to another line, where it is kept, its first line's ink gone.
    void theHighlighterAndTheLasso() {
        Bench bench;
        bench.ink()->setInking(true);
        bench.ink()->setTool("highlighter");
        bench.desk.settle();
        QCOMPARE(bench.surface()->tool(), InkSurface::Tool::Pen);
        QVERIFY(bench.surface()->highlighter() && !bench.surface()->pressure());
        const QRectF at = bench.word("storm");
        bench.stroke(QPointF(at.left(), at.center().y()), QPointF(at.right(), at.center().y()));
        QTRY_COMPARE(bench.kept().size(), 1);
        const Ink::Stroke lit = bench.kept().first().strokes.front();
        QVERIFY(lit.highlighter && !lit.pressured && lit.color.alphaF() < 0.5);

        bench.ink()->setTool("lasso");
        QTRY_COMPARE(bench.surface()->tool(), InkSurface::Tool::Lasso);
        const qint64 before = bench.kept().first().id;
        bench.surface()->selection()->select({lit.id});
        const QRectF other = bench.word("captain");
        bench.surface()->selection()->moveBy(other.left() - at.left(), other.center().y() - at.center().y());
        QTest::qWait(bench.surface()->batchPause() + 150);
        QTRY_VERIFY(!bench.kept().isEmpty() && bench.kept().first().id != before);
        QVERIFY2(bench.kept().first().passage.quote.contains("captain"), qPrintable(bench.kept().first().passage.quote));
        QCOMPARE(bench.kept().size(), 1);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // A finger writes in the margin while no pen has been seen, as on a PDF
    // (#134); with fingers off it scrolls the page and writes nothing.
    void aFingerWritesAsItDoesOnThePages() {
        Bench bench("en", {}, true);
        bench.key(Qt::Key_D);
        QTRY_VERIFY(bench.ink()->inking());
        bench.desk.settle();
        bench.surface()->rules() = Ink::InputRules{};
        bench.ink()->setFingers("auto");
        const QRectF at = bench.word("lantern");
        const QRectF column = bench.columnInScene();
        bench.swipe(QPointF(column.left() - 34, at.center().y()), QPointF(column.left() - 10, at.center().y() + 8));
        QTRY_COMPARE(bench.kept().size(), 1);
        QCOMPARE(bench.kept().first().kind, QString("margin-ink"));
        bench.ink()->setFingers("off");
        bench.swipe(QPointF(column.left() - 34, at.center().y() + 60), QPointF(column.left() - 10, at.center().y() + 20));
        QCOMPARE(bench.kept().size(), 1);
        QCOMPARE(bench.kept().first().strokes.size(), std::size_t{1});
        bench.ink()->setFingers("auto");
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The first ink is the text's colour on any theme, the others lighter on
    // a dark page; kept as the inks they are.
    void theInksAreDrawnInTheThemesColours() {
        for (const QByteArray &colors : {QByteArray(), lightTheme}) {
            Bench bench("en", colors);
            bench.ink()->setInking(true);
            bench.desk.settle();
            const QRectF at = bench.word("lantern");
            bench.stroke(QPointF(at.left(), at.bottom() - 1), QPointF(at.right(), at.bottom() - 1));
            QTRY_COMPARE(bench.kept().size(), 1);
            const Ink::Stroke kept = bench.kept().first().strokes.front();
            QCOMPARE(kept.color.name(), QString("#1a1a1a"));
            const Ink::Stroke *shown = bench.surface()->sheet().find(kept.id);
            QVERIFY(shown);
            QCOMPARE(shown->color.name(), bench.lineInk()->foreground().name());
            bench.desk.shot(colors.isEmpty() ? "lineink-dark" : "lineink-light");
        }
    }

    // Beside the highlights, the chapters with ink, each with the words of its
    // first line; Delete erases a chapter's, as a step to undo. Ink written on
    // another computer (a sync) comes onto the page shown.
    void theListShowsTheChaptersWithInkAndASyncsInkComes() {
        Bench bench;
        bench.ink()->setInking(true);
        bench.desk.settle();
        bench.writeBeside("lantern");
        QTRY_COMPARE(bench.lineInk()->list().size(), 1);
        bench.key(Qt::Key_B, Qt::ShiftModifier);
        QTRY_VERIFY(bench.desk.shows("inkRow_0"));
        QVERIFY(bench.desk.shows("inkHeading"));
        bench.desk.shot("lineink-list");
        bench.desk.find("highlightsList")->setProperty("currentIndex", 0);
        QMetaObject::invokeMethod(bench.desk.find("annotationsList"), "takeFocus");
        bench.key(Qt::Key_Delete);
        QTRY_VERIFY(bench.kept().isEmpty());
        QCOMPARE(bench.surface()->count(), 0);
        QVERIFY(bench.lineInk()->undo());
        QTRY_COMPARE(bench.kept().size(), 2);
        QCOMPARE(bench.surface()->count(), 2);

        const QString chapter = bench.desk.reader->sectionText();
        const auto from = chapter.indexOf("captain");
        const FoundPassage captain{.passage = Passage::of(chapter, 0, from, from + 7), .confidence = 1};
        Ink::Stroke theirs{
            .id = "theirs", .color = QColor("#1d4ed8"), .width = 2.4, .pressured = false, .tilted = false, .began = 5, .points = {}};
        for (int i = 0; i <= 8; ++i)
            theirs.points.push_back({.at = QPointF(i * 10, 104), .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = 0});
        QVERIFY(LineInk::make(*bench.desk.backend->library(), bench.book, QString::fromLatin1(LineInk::linesKind), captain, {}, {theirs}));
        bench.desk.backend->reload();
        QTRY_COMPARE(bench.surface()->count(), 3);
        QVERIFY(bench.surface()->sheet().find(QString("theirs")));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Every state at the window's sizes: writing with the bar, and the list
    // with a chapter of ink, in English, German on a light theme, Arabic (the
    // margins mirrored) and German under fingers; and in type too large for the margin.
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
        Bench bench(language, colors, touch);
        bench.ink()->setInking(true);
        bench.desk.settle();
        bench.writeBeside("lantern");
        QTRY_COMPARE(bench.kept().size(), 2);
        bench.ink()->setInking(false);
        QStringList problems;
        QObject *view = bench.desk.find("readerView");
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
            bench.desk.shot(QString("lineink-%1%2-%3-%4x%5")
                                .arg(QString::fromLatin1(language), touch ? "-touch" : "", state)
                                .arg(bench.desk.window->width())
                                .arg(bench.desk.window->height()));
        };
        for (const QSize &size : dialogSizes()) {
            QVERIFY(bench.desk.resize(size));
            bench.ink()->setInking(true);
            look("writing");
            bench.ink()->setTool("lasso");
            look("lasso");
            bench.ink()->setTool("pen");
            bench.ink()->setInking(false);
            QMetaObject::invokeMethod(view, "showList", Q_ARG(QVariant, "annotations"));
            look("list");
            view->setProperty("showIndex", false);
        }
        bench.desk.settings->setOption("font_size", 34);
        look("large-type");
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

private:
    static bool noHighlights(Bench &bench) { return bench.desk.backend->library()->annotations(bench.book, "highlight").isEmpty(); }
};

QTEST_MAIN(LineInkViewTests)
#include "lineinkview_tests.moc"
