// Book ink in the reader (#83), the whole window on a made-up library: the
// pen (synthetic tablet events, as a touchscreen laptop's pen sends them)
// writes on a PDF's page and on a comic's, while the reader writes on them;
// the ink is drawn where it was put at every zoom and scroll, is erased with
// the pen's other end, undone and done again a pause of the pen at a time,
// put away and shown, listed beside the page; a page of thousands of strokes
// draws within a frame; and every state of it is laid out at the window's
// sizes, in English, German, Arabic and under fingers.
//
// OMABOOK_SHOTS=<folder> saves a picture of each, to look at by eye.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QSGRendererInterface>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include <algorithm>
#include <functional>

#include "bookink.h"
#include "desk.h"
#include "highlights.h"
#include "inksurface.h"
#include "readingink.h"

namespace {

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &device(QPointingDevice::PointerType type) {
    static QPointingDevice pen{"pen", 8901, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    static QPointingDevice eraser{
        "pen eraser", 8902, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&pen);
        QWindowSystemInterface::registerInputDevice(&eraser);
        return true;
    }();
    return type == QPointingDevice::PointerType::Eraser ? eraser : pen;
}

// A book of fixed pages: a PDF with words, or a comic of white pages.
QString bookFile(const QString &folder, const QString &format) {
    const QString path = folder + "/The Ferry." + format;
    const bool made = format == "pdf"
                          ? Fixtures::pdf(path, "The Ferry", {"The harbour master counted the crates twice.", "Mira watched the lantern."})
                          : Fixtures::zip(path, {{"001.png", Fixtures::picture(800, 1200, Qt::white)},
                                                 {"002.png", Fixtures::picture(800, 1200, Qt::white)}});
    return made ? path : QString();
}

// A line of handwriting in ink units, for a page put together before it opens.
Ink::Stroke line(QPointF from, QPointF to, qint64 began) {
    Ink::Stroke stroke{
        .id = Ink::newId(), .color = QColor("#1a1a1a"), .width = 2.4, .pressured = false, .tilted = false, .began = began, .points = {}};
    for (int i = 0; i <= 24; ++i)
        stroke.points.push_back(
            {.at = from + ((to - from) * (i / 24.0)), .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = static_cast<quint32>(i * 8)});
    return stroke;
}

// The window, a book of pages open in it.
struct Bench {
    Desk desk;
    qint64 book = 0;

    Bench(const QString &format, const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false,
          const std::function<void(Library &, qint64)> &before = {})
        : desk(language, colors, touch) {
        const QString file = bookFile(desk.dir.path(), format);
        QVERIFY(!file.isEmpty());
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        if (before)
            before(*desk.backend->library(), book);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.settings->setOption("layout", "scroll");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(page() && page()->width() > 0 && surface());
        desk.settle();
    }

    [[nodiscard]] QQuickItem *page(int number = 0) const { return desk.item("pdfPage" + QString::number(number)); }
    [[nodiscard]] InkSurface *surface(int number = 0) const {
        return qobject_cast<InkSurface *>(desk.item("bookInk" + QString::number(number)));
    }
    [[nodiscard]] QObject *content() const { return desk.find("contentLoader")->property("item").value<QObject *>(); }
    [[nodiscard]] ReadingInk *ink() const { return desk.reader->ink(); }
    [[nodiscard]] std::vector<Ink::Stroke> kept(int number = 0) { return BookInk::strokes(*desk.backend->library(), book, number); }
    void key(int key, Qt::KeyboardModifiers modifiers = {}) const { QTest::keyClick(desk.window, static_cast<Qt::Key>(key), modifiers); }
    // A point of the window on the first page, by how far across and down it.
    [[nodiscard]] QPointF on(double across, double down, int number = 0) const {
        return page(number)->mapToScene(QPointF(across * page(number)->width(), down * page(number)->height()));
    }
    // Where an ink unit of the page is drawn now.
    [[nodiscard]] QPointF drawnAt(QPointF unit, int number = 0) const {
        return page(number)->mapToScene(unit * (page(number)->width() / BookInk::pageUnits));
    }

    ulong clock = 900'000;
    void penAt(QPointF at, bool down, QPointingDevice::PointerType type = QPointingDevice::PointerType::Pen) {
        clock += 8;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &device(type), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    // A stroke through these points of the window, and the pause after it, when the batch goes.
    void stroke(QPointF from, QPointF to, QPointingDevice::PointerType type = QPointingDevice::PointerType::Pen) {
        for (int i = 0; i <= 24; ++i)
            penAt(from + ((to - from) * (i / 24.0)), true, type);
        penAt(to, false, type);
        QTest::qWait(surface()->batchPause() + 150);
    }
    // Whether the window is dark at a point: ink on the white page.
    [[nodiscard]] bool dark(QPointF at) {
        desk.settle();
        const QImage window = desk.window->grabWindow();
        const QPoint pixel = (at * window.devicePixelRatio()).toPoint();
        return window.rect().contains(pixel) && window.pixelColor(pixel).lightness() < 110;
    }
    // Whether the window draws the ink's geometry: offscreen, the software renderer doesn't.
    [[nodiscard]] bool drawsGeometry() const {
        return desk.window->rendererInterface() != nullptr
               && desk.window->rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
    }
    [[nodiscard]] QString where(QPointF at) {
        const QImage window = desk.window->grabWindow();
        return QString("%1,%2 is %3").arg(at.x()).arg(at.y()).arg(window.pixelColor((at * window.devicePixelRatio()).toPoint()).name());
    }
};

QStringList idsOf(const std::vector<Ink::Stroke> &strokes) {
    QStringList ids;
    for (const Ink::Stroke &stroke : strokes)
        ids << stroke.id;
    ids.sort();
    return ids;
}

} // namespace

class BookInkViewTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // On a PDF's page and a comic's: D, and the pen writes on the page; the
    // stroke is kept on it in ink units, where it was put, and highlights
    // nothing. D again, and the pen is the gestures' once more.
    void thePenWritesOnAPdfAndAComicPage_data() {
        QTest::addColumn<QString>("format");
        QTest::newRow("pdf") << QString("pdf");
        QTest::newRow("comic") << QString("cbz");
    }
    void thePenWritesOnAPdfAndAComicPage() {
        QFETCH(QString, format);
        Bench bench(format);
        bench.key(Qt::Key_D);
        QTRY_VERIFY(bench.ink()->inking());
        QTRY_VERIFY(bench.desk.shows("inkBar"));
        QVERIFY(!bench.desk.item("penGestures")->isEnabled());
        InkSurface *surface = bench.surface();
        QVERIFY(!surface->readOnly() && surface->isVisible() && surface->isEnabled());
        const auto rectOf = [](const QQuickItem *item) {
            const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            return QString("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        };
        QVERIFY2(surface->contains(surface->mapFromScene(bench.on(0.2, 0.3))),
                 qPrintable(QString("the page's ink doesn't reach %1,%2: the page %3, the bar %4, the view %5")
                                .arg(bench.on(0.2, 0.3).x())
                                .arg(bench.on(0.2, 0.3).y())
                                .arg(rectOf(surface), rectOf(bench.desk.item("inkBar")), rectOf(bench.desk.item("pageReader")))));
        bench.penAt(bench.on(0.2, 0.3), true);
        QVERIFY2(surface->writing(), "the page's ink didn't take the pen");
        bench.penAt(bench.on(0.2, 0.3), false);
        QTest::qWait(surface->batchPause() + 150);
        QTRY_COMPARE(bench.kept().size(), std::size_t{1});
        QVERIFY(bench.ink()->undo());
        QVERIFY(bench.kept().empty());
        bench.stroke(bench.on(0.2, 0.3), bench.on(0.7, 0.3));
        QTRY_COMPARE(bench.kept().size(), std::size_t{1});
        const Ink::Stroke stroke = bench.kept().front();
        const double aspect = bench.page()->height() / bench.page()->width();
        QVERIFY2(std::abs(stroke.points.front().at.x() - 200) < 3 && std::abs(stroke.points.front().at.y() - (300 * aspect)) < 3,
                 qPrintable(QString("%1,%2").arg(stroke.points.front().at.x()).arg(stroke.points.front().at.y())));
        QVERIFY(std::abs(stroke.points.back().at.x() - 700) < 3);
        QVERIFY(Highlights::of(*bench.desk.backend->library(), bench.book).isEmpty());
        QCOMPARE(bench.ink()->pages().size(), 1);
        bench.desk.shot("ink-" + format);
        bench.key(Qt::Key_D);
        QTRY_VERIFY(!bench.ink()->inking());
        QVERIFY(bench.desk.item("penGestures")->isEnabled());
        QVERIFY(!bench.desk.shows("inkBar"));
        // Reading, the pen writes no ink.
        bench.stroke(bench.on(0.2, 0.6), bench.on(0.7, 0.6));
        QCOMPARE(bench.kept().size(), std::size_t{1});
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The ink is drawn where it was put, on the page, whatever the zoom and
    // wherever the page is scrolled to; off the line, the page is white.
    void inkStaysWhereItWasPutAtEveryZoomAndScroll_data() {
        QTest::addColumn<QString>("format");
        QTest::newRow("pdf") << QString("pdf");
        QTest::newRow("comic") << QString("cbz");
    }
    void inkStaysWhereItWasPutAtEveryZoomAndScroll() {
        QFETCH(QString, format);
        Bench bench(format);
        bench.ink()->setInking(true);
        bench.desk.settle();
        bench.surface()->setPenWidth(10);
        bench.stroke(bench.on(0.15, 0.25), bench.on(0.65, 0.25));
        QTRY_COMPARE(bench.kept().size(), std::size_t{1});
        const Ink::Stroke stroke = bench.kept().front();
        const QPointF middle = stroke.points.at(stroke.points.size() / 2).at;
        // Where the surface draws the stroke's middle is that point of the
        // page; and, where the scene graph draws geometry offscreen, it is
        // dark there and white off the line.
        const auto check = [&](const QString &state) {
            InkSurface *surface = bench.surface();
            QVERIFY2(surface && surface->sheet().find(stroke.id), qPrintable(state + ": the page's ink doesn't hold the stroke"));
            const QPointF at = bench.drawnAt(middle);
            const QPointF drawn = surface->mapToScene(middle * surface->inkScale());
            QVERIFY2(
                QLineF(at, drawn).length() < 0.5,
                qPrintable(QString("%1: drawn at %2,%3, not at %4,%5").arg(state).arg(drawn.x()).arg(drawn.y()).arg(at.x()).arg(at.y())));
            if (bench.drawsGeometry()) {
                QVERIFY2(bench.dark(at), qPrintable(state + ": the ink isn't there: " + bench.where(at)));
                const QPointF beside = bench.drawnAt(middle + QPointF(0, 60));
                QVERIFY2(!bench.dark(beside), qPrintable(state + ": the page is dark off the line: " + bench.where(beside)));
            }
            bench.desk.shot(QString("ink-%1-%2").arg(format, state));
        };
        check("as-drawn");
        QObject *content = bench.content();
        for (int step = 0; step < 3; ++step)
            QMetaObject::invokeMethod(content, "zoom", Q_ARG(QVariant, 1));
        QTRY_VERIFY(content->property("zoomFactor").toDouble() > 1.4);
        bench.desk.settle();
        check("zoomed-in");
        QMetaObject::invokeMethod(content, "scrollBy", Q_ARG(QVariant, 140));
        check("scrolled");
        for (int step = 0; step < 6; ++step)
            QMetaObject::invokeMethod(content, "zoom", Q_ARG(QVariant, -1));
        QTRY_VERIFY(content->property("zoomFactor").toDouble() < 0.8);
        bench.desk.settle();
        QMetaObject::invokeMethod(content, "scrollBy", Q_ARG(QVariant, -2000));
        check("zoomed-out");
        // The ink put away and shown again.
        bench.key(Qt::Key_D, Qt::ShiftModifier);
        QTRY_VERIFY(!bench.ink()->shown());
        QVERIFY(!bench.surface()->isVisible());
        QVERIFY(!bench.drawsGeometry() || !bench.dark(bench.drawnAt(middle)));
        bench.key(Qt::Key_D, Qt::ShiftModifier);
        QTRY_VERIFY(bench.ink()->shown());
        QVERIFY(bench.surface()->isVisible());
        QVERIFY(!bench.drawsGeometry() || bench.dark(bench.drawnAt(middle)));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The pen's other end erases what it goes over, kept as the stroke's
    // removal and its pieces' addition; Ctrl+Z undoes that, and the stroke
    // before it, and Ctrl+Shift+Z does them again, each in the log too.
    void thePensOtherEndErasesAndUndoGoesABatchAtATime() {
        Bench bench("pdf");
        bench.ink()->setInking(true);
        bench.desk.settle();
        bench.stroke(bench.on(0.2, 0.3), bench.on(0.7, 0.3));
        bench.stroke(bench.on(0.2, 0.5), bench.on(0.7, 0.5));
        QTRY_COMPARE(bench.kept().size(), std::size_t{2});
        const QStringList drawn = idsOf(bench.kept());
        bench.stroke(bench.on(0.45, 0.2), bench.on(0.45, 0.4), QPointingDevice::PointerType::Eraser);
        QTRY_COMPARE(bench.kept().size(), std::size_t{3});
        QVERIFY(idsOf(bench.kept()) != drawn);
        QCOMPARE(bench.surface()->count(), 3);
        bench.desk.shot("ink-erased");

        bench.key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(idsOf(bench.kept()), drawn);
        QCOMPARE(bench.surface()->count(), 2);
        bench.key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(bench.kept().size(), std::size_t{1});
        QCOMPARE(bench.surface()->count(), 1);
        bench.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(idsOf(bench.kept()), drawn);
        bench.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(bench.kept().size(), std::size_t{3});
        QCOMPARE(bench.surface()->count(), 3);
        QVERIFY(!bench.ink()->canRedo());
        // The bar's eraser, and the pen's own end erases.
        bench.ink()->setErasing(true);
        bench.stroke(bench.on(0.1, 0.5), bench.on(0.9, 0.5));
        QTRY_COMPARE(bench.kept().size(), std::size_t{2});
        bench.ink()->setErasing(false);
        // All of it undone: the page has no ink, and nothing is listed.
        while (bench.ink()->canUndo())
            QVERIFY(bench.ink()->undo());
        QVERIFY(bench.kept().empty());
        QVERIFY(bench.desk.backend->library()->annotations(bench.book, "ink").isEmpty());
        QVERIFY(bench.ink()->pages().isEmpty());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Beside the page, under the highlights: each page with ink, the part
    // of it the ink covers; a click goes there, Delete erases it, as a step
    // to undo. Ink written elsewhere (a sync) comes onto the page shown.
    void theListShowsThePagesWithInk() {
        Bench bench("pdf");
        bench.ink()->setInking(true);
        bench.desk.settle();
        bench.stroke(bench.on(0.2, 0.3), bench.on(0.7, 0.35));
        QTRY_COMPARE(bench.ink()->pages().size(), 1);
        const qint64 id = bench.ink()->pages().first().toMap().value("id").toLongLong();
        bench.key(Qt::Key_B, Qt::ShiftModifier);
        QTRY_VERIFY(bench.desk.shows("inkRow_" + QString::number(id)));
        QVERIFY(bench.desk.shows("inkHeading"));
        QTRY_VERIFY(bench.desk.shows("inkThumb0"));
        bench.desk.shot("ink-list");
        bench.desk.find("highlightsList")->setProperty("currentIndex", 0);
        QMetaObject::invokeMethod(bench.desk.find("annotationsList"), "takeFocus");
        bench.key(Qt::Key_Delete);
        QTRY_VERIFY(bench.kept().empty());
        QTRY_VERIFY(!bench.desk.shows("inkRow_" + QString::number(id)));
        QCOMPARE(bench.surface()->count(), 0);
        QVERIFY(bench.ink()->undo());
        QTRY_COMPARE(bench.kept().size(), std::size_t{1});
        QCOMPARE(bench.surface()->count(), 1);

        // Another computer's stroke on the page, as a sync brings it.
        const auto written =
            BookInk::write(*bench.desk.backend->library(), bench.book, 0, {.added = {line({100, 900}, {800, 900}, 5)}, .removed = {}});
        QVERIFY2(written, qPrintable(Fixtures::whyNot(written)));
        bench.desk.backend->reload();
        QTRY_COMPARE(bench.surface()->count(), 2);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // A page of 2,000 strokes: put together once when it opens, then each
    // frame while the pen writes redraws only the line being written.
    void aPageOfTwoThousandStrokesDrawsWithinAFrame() {
        Bench bench("pdf", "en", {}, false, [](Library &library, qint64 book) {
            Ink::Change many;
            for (int i = 0; i < 2000; ++i)
                many.added.push_back(
                    line(QPointF(10 + (i % 10) * 98, 10 + (i / 10) * 7), QPointF(80 + (i % 10) * 98, 14 + (i / 10) * 7), i));
            QVERIFY(BookInk::write(library, book, 0, many));
        });
        QTRY_COMPARE(bench.surface()->count(), 2000);
        bench.desk.settle();
        const double opened = bench.surface()->frameTime();
        bench.ink()->setInking(true);
        bench.desk.settle();
        double writing = 0;
        bench.penAt(bench.on(0.3, 0.1), true);
        for (int i = 1; i <= 30; ++i) {
            bench.penAt(bench.on(0.3 + (i * 0.01), 0.1 + (i * 0.002)), true);
            bench.desk.settle(1);
            writing = std::max(writing, bench.surface()->frameTime());
        }
        bench.penAt(bench.on(0.6, 0.16), false);
        QTRY_COMPARE(bench.surface()->count(), 2001);
        qInfo() << "2000 strokes on a page: put together in" << opened << "ms; while writing, at most" << writing << "ms";
        // Well within a frame (16 ms), on a machine several times slower than this one.
        QVERIFY2(writing < 4, qPrintable(QString::number(writing)));
        QVERIFY2(opened < 250, qPrintable(QString::number(opened)));
        bench.desk.shot("ink-2000-strokes");
    }

    // Every state of it at the window's sizes: the ink's bar while writing,
    // and the list with a page of ink, in English, German on a light theme,
    // Arabic, and German under fingers.
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
        Bench bench("pdf", language, colors, touch, [](Library &library, qint64 book) {
            QVERIFY(BookInk::write(library, book, 0,
                                   {.added = {line({150, 300}, {700, 340}, 1), line({200, 420}, {640, 400}, 2)}, .removed = {}}));
        });
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
            bench.desk.shot(QString("ink-%1%2-%3-%4x%5")
                                .arg(QString::fromLatin1(language), touch ? "-touch" : "", state)
                                .arg(bench.desk.window->width())
                                .arg(bench.desk.window->height()));
        };
        for (const QSize &size : dialogSizes()) {
            QVERIFY(bench.desk.resize(size));
            bench.ink()->setInking(true);
            look("writing");
            bench.ink()->setErasing(true);
            look("erasing");
            bench.ink()->setErasing(false);
            bench.ink()->setInking(false);
            QMetaObject::invokeMethod(view, "showList", Q_ARG(QVariant, "annotations"));
            look("list");
            view->setProperty("showIndex", false);
            view->setProperty("showBar", true);
            look("bar");
            view->setProperty("showBar", false);
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }
};

QTEST_MAIN(BookInkViewTests)
#include "bookinkview_tests.moc"
