// Writing with a finger (#134), the whole window on a made-up library: with D
// on, one finger writes on a PDF's page and a comic's while no pen has been
// seen, and the page holds still under it, a tap of it turning nothing; two
// fingers scroll and zoom the pages and write nothing; once a pen is seen a
// finger scrolls, unless the writing bar's choice (kept in the settings) says
// a finger always writes, or never. A handwritten margin note being edited
// takes fingers the same way. Fingers and the pen are synthetic touch and
// tablet events with their own times, as a touchscreen laptop sends them.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include "bookink.h"
#include "desk.h"
#include "hyphenation.h"
#include "inksurface.h"
#include "marginnotes.h"
#include "readingink.h"

namespace {

using State = QEventPoint::State;

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &pen() {
    static QPointingDevice device{"pen", 9301, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
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

// A book of fixed pages: a PDF with words, or a comic of white pages.
QString bookFile(const QString &folder, const QString &format) {
    const QString path = folder + "/The Ferry." + format;
    const bool made = format == "pdf"
                          ? Fixtures::pdf(path, "The Ferry", {"The harbour master counted the crates twice.", "Mira watched the lantern."})
                          : Fixtures::zip(path, {{"001.png", Fixtures::picture(800, 1200, Qt::white)},
                                                 {"002.png", Fixtures::picture(800, 1200, Qt::white)}});
    return made ? path : QString();
}

// The window, and fingers and a pen on it, all on one clock.
struct Hands {
    Desk desk{"en", {}, false};
    ulong clock = 900'000;

    [[nodiscard]] QWindowSystemInterface::TouchPoint finger(State state, QPointF at, int id = 0) const {
        QWindowSystemInterface::TouchPoint point;
        point.id = id;
        point.state = state;
        point.pressure = state == State::Released ? 0.0 : 1.0;
        const QPointF global = desk.window->mapToGlobal(at);
        point.area = QRectF(0, 0, 9, 9);
        point.area.moveCenter(global);
        const QRect screen = desk.window->screen()->geometry();
        point.normalPosition = QPointF(global.x() / screen.width(), global.y() / screen.height());
        return point;
    }
    void touches(const QList<QWindowSystemInterface::TouchPoint> &points) {
        clock += 12;
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(desk.window, clock, fingers(), points);
    }
    // One finger, from one point of the window to another.
    void swipe(QPointF from, QPointF to) {
        touches({finger(State::Pressed, from)});
        for (int i = 1; i <= 12; ++i)
            touches({finger(State::Updated, from + ((to - from) * (i / 12.0)))});
        touches({finger(State::Released, to)});
    }
    void tap(QPointF at) {
        touches({finger(State::Pressed, at)});
        touches({finger(State::Released, at)});
    }
    // Two fingers, each from its point by `moved`, and the second further from the first by `spread` across.
    void twoFingers(QPointF first, QPointF second, QPointF moved, double spread = 0) {
        touches({finger(State::Pressed, first)});
        touches({finger(State::Stationary, first), finger(State::Pressed, second, 1)});
        for (int i = 1; i <= 12; ++i) {
            const double t = i / 12.0;
            touches({finger(State::Updated, first + (moved * t) - QPointF(spread * t / 2, 0)),
                     finger(State::Updated, second + (moved * t) + QPointF(spread * t / 2, 0), 1)});
        }
        touches({finger(State::Released, first + moved - QPointF(spread / 2, 0)),
                 finger(State::Released, second + moved + QPointF(spread / 2, 0), 1)});
    }
    void penAt(QPointF at, bool down) {
        clock += 8;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    void penStroke(QPointF from, QPointF to) {
        for (int i = 0; i <= 12; ++i)
            penAt(from + ((to - from) * (i / 12.0)), true);
        penAt(to, false);
    }
    // Long enough after the pen that a finger is no palm.
    void later() { clock += 2000; }
    void click(const QString &name) const {
        QQuickItem *item = desk.item(name);
        QVERIFY2(item && item->isVisible(), qPrintable(name));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
};

// A PDF or a comic open, scrolling, and D pressed.
struct Pages : Hands {
    qint64 book = 0;

    explicit Pages(const QString &format) {
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
        QTRY_VERIFY(page() && page()->width() > 0 && surface());
        desk.settle();
        QTest::keyClick(desk.window, Qt::Key_D);
        QTRY_VERIFY(desk.reader->ink()->inking());
        desk.settle();
        // As if no pen had ever been seen on this computer.
        for (int number = 0; number < 2; ++number)
            if (InkSurface *one = surface(number))
                one->rules() = Ink::InputRules{};
    }
    [[nodiscard]] QQuickItem *page(int number = 0) const { return desk.item("pdfPage" + QString::number(number)); }
    [[nodiscard]] InkSurface *surface(int number = 0) const {
        return qobject_cast<InkSurface *>(desk.item("bookInk" + QString::number(number)));
    }
    [[nodiscard]] QQuickItem *list() const { return desk.item("pageReader"); }
    [[nodiscard]] double scrolled() const { return list()->property("contentY").toDouble(); }
    [[nodiscard]] double zoom() const { return list()->property("zoomFactor").toDouble(); }
    [[nodiscard]] std::size_t kept() { return BookInk::strokes(*desk.backend->library(), book, 0).size(); }
    // A point of the window on the first page, by how far across and down it.
    [[nodiscard]] QPointF on(double across, double down) const {
        return page()->mapToScene(QPointF(across * page()->width(), down * page()->height()));
    }
    // Until a flick of the pages is over: a touch while they move stops them.
    void still() const { QTRY_VERIFY(!list()->property("moving").toBool()); }
    // Back to the first page's top, under the writing bar.
    void toTop() {
        QMetaObject::invokeMethod(list(), "toStart");
        desk.settle();
    }
    // The pen's pause, when what it wrote is kept.
    void pause() const { QTest::qWait(surface()->batchPause() + 150); }
};

// Handwriting as a pen draws it, in a note's ink units.
std::vector<Ink::Stroke> writing() {
    Ink::Stroke stroke{
        .id = Ink::newId(), .color = Qt::blue, .width = 2, .pressured = true, .tilted = false, .began = 1'700'000'000'000, .points = {}};
    for (int i = 0; i < 12; ++i)
        stroke.points.push_back({.at = {100.0 + (i % 2) * 6, 300.0 + i * 2}, .pressure = 0.6F, .xTilt = 0, .yTilt = 0, .ms = 0});
    return {stroke};
}

[[nodiscard]] QString textOf(QQuickItem *text) {
    QString words;
    QMetaObject::invokeMethod(text, "getText", Q_RETURN_ARG(QString, words), Q_ARG(int, 0), Q_ARG(int, text->property("length").toInt()));
    return words;
}

[[nodiscard]] QPointF middleOf(const QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }

} // namespace

class FingerInkTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // No pen seen: a finger writes on the page, which holds still under it,
    // and a tap of it at the page's edge is a dot, not a turn of the page.
    void oneFingerWritesWhileNoPenIsSeen_data() {
        QTest::addColumn<QString>("format");
        QTest::newRow("pdf") << QString("pdf");
        QTest::newRow("comic") << QString("cbz");
    }
    void oneFingerWritesWhileNoPenIsSeen() {
        QFETCH(QString, format);
        Pages pages(format);
        QCOMPARE(pages.desk.reader->ink()->fingers(), QString("auto"));
        QCOMPARE(pages.surface()->fingers(), InkSurface::FingerUse::Auto);
        const double before = pages.scrolled();
        pages.swipe(pages.on(0.2, 0.3), pages.on(0.6, 0.15));
        pages.pause();
        QTRY_COMPARE(pages.kept(), std::size_t{1});
        QCOMPARE(pages.scrolled(), before);
        const bool bar = pages.desk.find("readerView")->property("showBar").toBool();
        pages.tap(pages.on(0.9, 0.5));
        pages.pause();
        QTRY_COMPARE(pages.kept(), std::size_t{2});
        QCOMPARE(pages.scrolled(), before);
        QCOMPARE(pages.desk.find("readerView")->property("showBar").toBool(), bar);
        QVERIFY2(pages.desk.warnings.isEmpty(), qPrintable(pages.desk.warnings.join('\n')));
    }

    // Two fingers scroll the pages and spread to zoom them; they write nothing.
    void twoFingersScrollAndZoom_data() { oneFingerWritesWhileNoPenIsSeen_data(); }
    void twoFingersScrollAndZoom() {
        QFETCH(QString, format);
        Pages pages(format);
        const double before = pages.scrolled();
        pages.twoFingers(pages.on(0.3, 0.5), pages.on(0.6, 0.5), QPointF(0, -150));
        QVERIFY2(std::abs(pages.scrolled() - (before + 150)) < 2,
                 qPrintable(QString("scrolled from %1 to %2").arg(before).arg(pages.scrolled())));
        const double zoom = pages.zoom();
        pages.twoFingers(pages.on(0.3, 0.5), pages.on(0.5, 0.5), {}, 200);
        QVERIFY2(pages.zoom() > zoom * 1.5, qPrintable(QString::number(pages.zoom())));
        pages.pause();
        QCOMPARE(pages.kept(), std::size_t{0});
        QVERIFY(pages.desk.reader->ink()->pages().isEmpty());
        QVERIFY2(pages.desk.warnings.isEmpty(), qPrintable(pages.desk.warnings.join('\n')));
    }

    // A pen seen, a finger scrolls the page; unless the writing bar says a
    // finger writes, which it then does with a pen seen or not; or never,
    // when it scrolls with no pen seen. The choice is kept.
    void aPenSeenAFingerScrollsUnlessAsked_data() { oneFingerWritesWhileNoPenIsSeen_data(); }
    void aPenSeenAFingerScrollsUnlessAsked() {
        QFETCH(QString, format);
        Pages pages(format);
        pages.penStroke(pages.on(0.2, 0.2), pages.on(0.5, 0.2));
        pages.pause();
        QTRY_COMPARE(pages.kept(), std::size_t{1});
        pages.later();
        double before = pages.scrolled();
        pages.swipe(pages.on(0.4, 0.6), pages.on(0.4, 0.3));
        QTRY_VERIFY2(pages.scrolled() > before + 50, qPrintable(QString("%1 to %2").arg(before).arg(pages.scrolled())));
        pages.still();
        pages.pause();
        QCOMPARE(pages.kept(), std::size_t{1});

        pages.click("inkFingers");
        QTRY_COMPARE(pages.desk.settings->inkFingers(), QString("on"));
        QCOMPARE(pages.surface()->fingers(), InkSurface::FingerUse::Draw);
        QVERIFY(pages.surface()->penSeen());
        pages.toTop();
        before = pages.scrolled();
        pages.swipe(pages.on(0.2, 0.4), pages.on(0.6, 0.45));
        pages.pause();
        QTRY_COMPARE(pages.kept(), std::size_t{2});
        QCOMPARE(pages.scrolled(), before);

        pages.click("inkFingers");
        QTRY_COMPARE(pages.desk.settings->inkFingers(), QString("off"));
        QCOMPARE(pages.surface()->fingers(), InkSurface::FingerUse::Scroll);
        pages.surface()->rules() = Ink::InputRules{};
        before = pages.scrolled();
        pages.swipe(pages.on(0.4, 0.6), pages.on(0.4, 0.3));
        QTRY_VERIFY(pages.scrolled() > before + 50);
        pages.still();
        pages.pause();
        QCOMPARE(pages.kept(), std::size_t{2});
        // Kept: read again from the file, it is what was chosen.
        QCOMPARE(Settings(pages.desk.dir.filePath("config.json")).inkFingers(), QString("off"));
        pages.click("inkFingers");
        QTRY_COMPARE(pages.desk.settings->inkFingers(), QString("auto"));
        QCOMPARE(pages.surface()->fingers(), InkSurface::FingerUse::Auto);
        QVERIFY2(pages.desk.warnings.isEmpty(), qPrintable(pages.desk.warnings.join('\n')));
    }

    // A handwritten margin note (#90) being edited: a finger writes on it
    // while no pen is seen, two fingers scroll the page it is beside, and a
    // pen seen, a finger scrolls, unless a finger is to write.
    void aHandwrittenNoteTakesFingersAsThePagesDo() {
        Hands hands;
        const QList<qint64> ids = stock(hands.desk);
        QVERIFY2(hands.desk.show(), qPrintable(hands.desk.warnings.join('\n')));
        QVERIFY(hands.desk.resize(QSize(1600, 900)));
        hands.desk.settings->setPages(1);
        hands.desk.reader->format()->set("layout", "scroll", false);
        hands.desk.reader->openBook(ids.first());
        QTRY_VERIFY(hands.desk.reader->isOpen() && hands.desk.shows("flowText"));
        hands.desk.settle();
        auto *notes = hands.desk.find("readingNotes")->property("notes").value<MarginNotes *>();
        QVERIFY(notes);
        const QString shown = textOf(hands.desk.item("flowText"));
        const Hyphenation::Plain plain(shown);
        const qsizetype at = plain.text().indexOf("Paragraph 2 of a long chapter");
        QVERIFY(at >= 0);
        const std::optional<qint64> id = notes->writeInk(shown, plain.toShown(at), plain.toShownEnd(at + 29), writing(), 0.5);
        QVERIFY(id);
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = hands.desk.item(QString("marginNote_%1").arg(*id))) && card->isVisible());
        hands.click(QString("marginNote_%1").arg(*id));
        QTRY_VERIFY(card->property("editing").toBool());
        auto *ink = qobject_cast<InkSurface *>(card->findChild<QQuickItem *>("noteInk"));
        QVERIFY(ink);
        ink->rules() = Ink::InputRules{};
        QCOMPARE(ink->fingers(), InkSurface::FingerUse::Auto);
        const int before = ink->count();
        QPointF middle = middleOf(ink);
        hands.swipe(middle - QPointF(30, 0), middle + QPointF(30, 6));
        QTRY_COMPARE(ink->count(), before + 1);

        QQuickItem *page = hands.desk.item("flowReader");
        const double top = page->property("contentY").toDouble();
        hands.twoFingers(middle - QPointF(40, 0), middle + QPointF(40, 0), QPointF(0, -120));
        QVERIFY2(std::abs(page->property("contentY").toDouble() - (top + 120)) < 2,
                 qPrintable(QString("%1 to %2").arg(top).arg(page->property("contentY").toDouble())));
        QCOMPARE(ink->count(), before + 1);
        QVERIFY(!ink->touched());

        // A pen seen (hovering, far from the note): a finger scrolls.
        hands.penAt(QPointF(40, 860), false);
        QVERIFY(ink->penSeen());
        hands.later();
        middle = middleOf(ink);
        const double scrolledTo = page->property("contentY").toDouble();
        hands.swipe(middle, middle - QPointF(0, 80));
        QTRY_VERIFY(page->property("contentY").toDouble() > scrolledTo + 30);
        QCOMPARE(ink->count(), before + 1);

        // A finger asked to write does.
        hands.desk.reader->ink()->setFingers("on");
        QCOMPARE(ink->fingers(), InkSurface::FingerUse::Draw);
        hands.desk.settle();
        middle = middleOf(ink);
        hands.swipe(middle - QPointF(30, 0), middle + QPointF(30, 6));
        QTRY_COMPARE(ink->count(), before + 2);
        QVERIFY2(hands.desk.warnings.isEmpty(), qPrintable(hands.desk.warnings.join('\n')));
    }
};

QTEST_MAIN(FingerInkTests)
#include "fingerink_tests.moc"
