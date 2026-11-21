// The ink surface (#78), driven as a device drives it: pen, eraser end,
// finger and mouse events come through Qt's window system interface with
// their own times, contact sizes and proximity, as the compositor sends
// them. A stand-in notebook plugin keeps the strokes in its part of the
// change log and draws them back; a page of 2,000 strokes is timed.
//
// OMABOOK_SHOTS=<folder> saves what the surface drew, to look at by eye.

#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPointingDevice>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include <array>
#include <cmath>
#include <memory>

#include "changelog.h"
#include "fixtures.h"
#include "inkcodec.h"
#include "inkmesh.h"
#include "inksurface.h"
#include "library.h"
#include "plugindata.h"
#include "scriptplugin.h"
#include "scriptplugins.h"

using namespace Qt::StringLiterals;

namespace {

using Fixtures::whyNot;

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::XTilt
                         | QInputDevice::Capability::YTilt | QInputDevice::Capability::Hover;

// The devices of a touchscreen laptop with a pen, as Qt knows them on Wayland:
// the pen's two ends are tools of their own; one pen says no pressure.
struct Devices {
    QPointingDevice pen{u"pen"_s, 7801, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 3};
    QPointingDevice eraser{u"pen eraser"_s, 7802, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser, penCaps, 1, 3};
    QPointingDevice plainPen{u"pen without pressure"_s,
                             7803,
                             QInputDevice::DeviceType::Stylus,
                             QPointingDevice::PointerType::Pen,
                             QInputDevice::Capability::Position,
                             1,
                             2};
    QPointingDevice *finger = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen,
                                                       QInputDevice::Capability::Position | QInputDevice::Capability::Area);

    Devices() {
        for (const QPointingDevice *device : {&pen, &eraser, &plainPen})
            QWindowSystemInterface::registerInputDevice(device);
    }
};

Devices &devices() {
    static Devices all;
    return all;
}

constexpr const char *page = R"(import QtQuick
import Omabook

Item {
    width: 640
    height: 480
    // What is under the surface: what a touch passed on reaches.
    MultiPointTouchArea {
        objectName: "under"
        property int presses: 0
        anchors.fill: parent
        onPressed: presses += 1
    }
    InkSurface {
        objectName: "ink"
        anchors.fill: parent
        batchPause: 30
    }
}
)";

// A surface on a Flickable, as a book's page is (#134): what two fingers move.
constexpr const char *scrolled = R"(import QtQuick
import Omabook

Flickable {
    objectName: "under"
    width: 640
    height: 480
    contentWidth: 640
    contentHeight: 2000
    contentY: 500
    InkSurface {
        objectName: "ink"
        width: 640
        height: 2000
        batchPause: 30
    }
}
)";

// A surface in a window, and the batches it sends.
class Bench {
public:
    explicit Bench(const QString &qml = QString::fromUtf8(page)) {
        registerInkTypes();
        devices();
        QVERIFY(Fixtures::write(m_dir.filePath("Page.qml"), qml.toUtf8()));
        m_view.setResizeMode(QQuickView::SizeRootObjectToView);
        m_view.setSource(QUrl::fromLocalFile(m_dir.filePath("Page.qml")));
        QVERIFY2(m_view.status() == QQuickView::Ready,
                 qPrintable(m_view.errors().isEmpty() ? QString() : m_view.errors().first().toString()));
        m_view.resize(640, 480);
        m_view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_view));
        surface = m_view.rootObject()->findChild<InkSurface *>(u"ink"_s);
        under = m_view.rootObject()->findChild<QQuickItem *>(u"under"_s);
        QVERIFY(surface != nullptr);
        // Each test meets the surface as if no pen had ever been seen.
        surface->rules() = Ink::InputRules{};
        QObject::connect(surface, &InkSurface::inked, surface, [this](const QVariantMap &batch) { batches.append(batch); });
    }

    // A pen's event: down (a button held), moving, or up; hovering is moving with none.
    void pen(const QPointingDevice &device, QPointF at, double pressure, bool down) {
        clock += 4;
        QWindowSystemInterface::handleTabletEvent(&m_view, clock, &device, at, m_view.mapToGlobal(at), down ? Qt::LeftButton : Qt::NoButton,
                                                  pressure, 12.0, -7.0, 0.0, 0.0, 0.0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    // A line with a pen, from `from` to `to` in `steps`, the pressure going from one to the other.
    void stroke(const QPointingDevice &device, QPointF from, QPointF to, double fromPressure = 0.5, double toPressure = 0.5,
                int steps = 30) {
        for (int i = 0; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            pen(device, from + (to - from) * t, fromPressure + (toPressure - fromPressure) * t, true);
        }
        pen(device, to, 0.0, false);
    }
    void proximity(const QPointingDevice &device, bool near) {
        clock += 4;
        QWindowSystemInterface::handleTabletEnterLeaveProximityEvent(&m_view, clock, &device, near);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    // A finger's touch, its contact `diameter` pixels across (0: the device says none).
    [[nodiscard]] QWindowSystemInterface::TouchPoint finger(QEventPoint::State state, QPointF at, double diameter, int id = 1) const {
        QWindowSystemInterface::TouchPoint point;
        point.id = id;
        point.state = state;
        point.pressure = state == QEventPoint::State::Released ? 0.0 : 1.0;
        const QPointF global = m_view.mapToGlobal(at);
        point.area = QRectF(0, 0, diameter, diameter);
        point.area.moveCenter(global);
        const QRect screen = m_view.screen()->geometry();
        point.normalPosition = QPointF(global.x() / screen.width(), global.y() / screen.height());
        return point;
    }
    void touch(QEventPoint::State state, QPointF at, double diameter, int id = 1) { touches({finger(state, at, diameter, id)}); }
    // Every finger on the screen, in one event.
    void touches(const QList<QWindowSystemInterface::TouchPoint> &points) {
        clock += 8;
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(&m_view, clock, devices().finger, points);
    }
    void swipe(QPointF from, QPointF to, double diameter, double grownTo = -1) {
        touch(QEventPoint::State::Pressed, from, diameter);
        for (int i = 1; i <= 10; ++i)
            touch(QEventPoint::State::Updated, from + (to - from) * (i / 10.0), grownTo > 0 && i > 3 ? grownTo : diameter);
        touch(QEventPoint::State::Released, to, diameter);
    }
    // A closed path around `corners`, with the pen, the mouse or a finger.
    static QList<QPointF> around(const QList<QPointF> &corners) {
        QList<QPointF> path;
        for (qsizetype i = 0; i < corners.size(); ++i)
            for (int step = 0; step < 8; ++step)
                path.append(corners.at(i) + (corners.at((i + 1) % corners.size()) - corners.at(i)) * (step / 8.0));
        path.append(corners.front());
        return path;
    }
    void penLoop(const QList<QPointF> &corners) {
        for (const QPointF &at : around(corners))
            pen(devices().pen, at, 0.5, true);
        pen(devices().pen, corners.front(), 0.0, false);
    }
    void mouseLoop(const QList<QPointF> &corners) {
        const QList<QPointF> path = around(corners);
        QTest::mousePress(&m_view, Qt::LeftButton, {}, path.front().toPoint());
        for (const QPointF &at : path)
            QTest::mouseMove(&m_view, at.toPoint());
        QTest::mouseRelease(&m_view, Qt::LeftButton, {}, path.back().toPoint());
    }
    // Qt Quick holds a touch's moves until the next frame, and delivers the last: a frame for each.
    void touchLoop(const QList<QPointF> &corners) {
        const QList<QPointF> path = around(corners);
        touch(QEventPoint::State::Pressed, path.front(), 9);
        for (const QPointF &at : path) {
            touch(QEventPoint::State::Updated, at, 9);
            QSignalSpy drawn(&m_view, &QQuickWindow::frameSwapped);
            m_view.update();
            std::ignore = drawn.wait(200);
        }
        touch(QEventPoint::State::Released, path.back(), 9);
    }
    [[nodiscard]] int presses() const { return under->property("presses").toInt(); }
    // Waits for the pen's pause and answers the batches sent.
    QList<QVariantMap> pause() {
        QTest::qWait(surface->batchPause() + 40);
        return std::exchange(batches, {});
    }
    [[nodiscard]] bool frame() {
        QSignalSpy drawn(&m_view, &QQuickWindow::frameSwapped);
        surface->update();
        return drawn.wait(3000);
    }
    void shot(const QString &name) {
        if (const QString folder = qEnvironmentVariable("OMABOOK_SHOTS"); !folder.isEmpty())
            m_view.grabWindow().save(folder + "/" + name + ".png");
    }
    QImage grab() { return m_view.grabWindow(); }
    [[nodiscard]] bool rendersGeometry() const {
        return m_view.rendererInterface() != nullptr && m_view.rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
    }
    QQuickView &view() { return m_view; }

    InkSurface *surface = nullptr;
    QQuickItem *under = nullptr;
    QList<QVariantMap> batches;
    ulong clock = 100'000;

private:
    QTemporaryDir m_dir;
    QQuickView m_view;
};

[[nodiscard]] QSet<QString> setOf(const QStringList &ids) { return {ids.begin(), ids.end()}; }

// The strokes on the surface whose line begins at `at`, in ink units.
[[nodiscard]] QStringList beginningAt(const InkSurface &surface, QPointF at) {
    QStringList ids;
    for (const Ink::Stroke &stroke : surface.sheet().strokes())
        if (QLineF(stroke.points.front().at, at).length() < 0.01)
            ids.append(stroke.id);
    return ids;
}

[[nodiscard]] QStringList keys(const QVariantMap &batch, const char *part) {
    if (QString::fromLatin1(part) == u"add"_s)
        return batch.value(u"add"_s).toMap().keys();
    return batch.value(u"remove"_s).toStringList();
}

// A line of handwriting, as the frame test's page has 2,000 of.
Ink::Stroke handwriting(QPointF from, int count, int seed) {
    Ink::Stroke stroke{.id = Ink::newId(),
                       .color = QColor(0x22, 0x22, 0x33),
                       .width = 2.0,
                       .pressured = true,
                       .tilted = false,
                       .began = 1'795'000'000'000 + seed,
                       .points = {}};
    for (int i = 0; i < count; ++i) {
        const double t = i + seed;
        stroke.points.push_back(Ink::Point{.at = from + QPointF(i * 1.2, 5.0 * std::sin(t / 4.0)),
                                           .pressure = static_cast<float>(0.5 + 0.3 * std::sin(t / 7.0)),
                                           .ms = static_cast<quint32>(i * 4)});
    }
    return stroke;
}

// Triangles of a mesh, painted as the scene graph would draw them.
void paint(QPainter &painter, const Ink::Mesh &mesh) {
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::array corners{mesh.vertices.at(mesh.indices.at(i)), mesh.vertices.at(mesh.indices.at(i + 1)),
                                 mesh.vertices.at(mesh.indices.at(i + 2))};
        const int alpha = (corners[0].a + corners[1].a + corners[2].a) / 3;
        if (alpha == 0)
            continue;
        const auto unmultiplied = [&](quint8 value) { return std::min(255, value * 255 / std::max<int>(1, corners[0].a)); };
        painter.setBrush(QColor(unmultiplied(corners[0].r), unmultiplied(corners[0].g), unmultiplied(corners[0].b), alpha));
        const std::array<QPointF, 3> points{QPointF(corners[0].x, corners[0].y), QPointF(corners[1].x, corners[1].y),
                                            QPointF(corners[2].x, corners[2].y)};
        painter.drawPolygon(points.data(), 3);
    }
}

// The surface's strokes as their triangles say, painted, a selection being
// dragged where it goes, and the lasso's marks over them: what the scene
// graph would draw, where it draws no geometry (the software renderer).
QImage painted(InkSurface &surface, QSize size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    Ink::Mesh mesh;
    for (const Ink::Stroke &stroke : surface.sheet().strokes()) {
        const Ink::Stroke *moving = surface.selection()->lasso().moving(stroke.id);
        Ink::tessellate(moving != nullptr ? *moving : stroke, surface.inkScale(), 1.0, mesh);
        paint(painter, mesh);
    }
    surface.selection()->marks(mesh);
    paint(painter, mesh);
    return image;
}

// What the surface shows, saved to look at by eye.
void look(InkSurface &surface, const QString &name) {
    if (const QString folder = qEnvironmentVariable("OMABOOK_SHOTS"); !folder.isEmpty())
        painted(surface, QSize(640, 480)).save(folder + "/" + name + ".png");
}

} // namespace

class InkSurfaceTests : public QObject {
    Q_OBJECT

private slots:
    // The pen draws a stroke of its points, each with its pressure, tilt
    // and moment, in ink units; harder is wider. It goes out as one batch
    // when the pen pauses.
    void thePenDrawsWithPressure() {
        Bench bench;
        bench.surface->setInkScale(2.0);
        bench.stroke(devices().pen, QPointF(100, 100), QPointF(400, 100), 0.1, 0.9);
        QCOMPARE(bench.surface->count(), 1);
        const Ink::Stroke &stroke = bench.surface->sheet().strokes().front();
        QVERIFY(stroke.pressured && stroke.tilted);
        QVERIFY(stroke.points.size() > 25);
        QCOMPARE(stroke.points.front().at, QPointF(50, 50));
        QVERIFY(std::abs(stroke.points.back().at.x() - 200.0) < 0.01);
        QVERIFY(stroke.points.back().pressure > stroke.points.front().pressure + 0.6F);
        QCOMPARE(stroke.points.front().xTilt, 12.0F);
        QVERIFY(stroke.points.back().ms > stroke.points.front().ms);
        QVERIFY(Ink::widthAt(stroke, stroke.points.back()) > 2.5 * Ink::widthAt(stroke, stroke.points.front()));
        QVERIFY(bench.surface->penSeen());
        // Hovering draws nothing.
        bench.pen(devices().pen, QPointF(300, 300), 0.0, false);
        bench.pen(devices().pen, QPointF(320, 300), 0.0, false);
        QCOMPARE(bench.surface->count(), 1);
        const auto batches = bench.pause();
        QCOMPARE(batches.size(), 1);
        QCOMPARE(keys(batches.front(), "add"), QStringList{stroke.id});
        QVERIFY(keys(batches.front(), "remove").isEmpty());
        QVERIFY(bench.frame());
    }

    // A pen that says no pressure draws its width all along.
    void aPenWithoutPressureDrawsItsWidth() {
        Bench bench;
        bench.stroke(devices().plainPen, QPointF(100, 100), QPointF(300, 200), 0.0, 0.0);
        QCOMPARE(bench.surface->count(), 1);
        const Ink::Stroke &stroke = bench.surface->sheet().strokes().front();
        QVERIFY(!stroke.pressured && !stroke.tilted);
        QCOMPARE(Ink::widthAt(stroke, stroke.points.front()), bench.surface->penWidth());
    }

    // The pen's other end erases what it goes over, cutting a stroke in
    // two: one batch removes it and adds both pieces. Undo puts it back.
    void theEraserEndErasesPartOfAStroke() {
        Bench bench;
        bench.stroke(devices().pen, QPointF(100, 200), QPointF(500, 200));
        const QString line = bench.surface->sheet().strokes().front().id;
        QCOMPARE(bench.pause().size(), 1);
        bench.stroke(devices().eraser, QPointF(300, 120), QPointF(300, 280));
        QCOMPARE(bench.surface->count(), 2);
        const auto erased = bench.pause();
        QCOMPARE(erased.size(), 1);
        QCOMPARE(keys(erased.front(), "remove"), QStringList{line});
        QCOMPARE(keys(erased.front(), "add").size(), 2);
        bench.shot(u"ink-erased-in-part"_s);

        bench.surface->undo();
        QCOMPARE(bench.surface->count(), 1);
        QCOMPARE(bench.surface->sheet().strokes().front().id, line);
        const auto undone = bench.pause();
        QCOMPARE(undone.size(), 1);
        QCOMPARE(keys(undone.front(), "remove").size(), 2);
        QCOMPARE(keys(undone.front(), "add"), QStringList{line});
        bench.surface->redo();
        QCOMPARE(bench.surface->count(), 2);
    }

    // Without an eraser end, the eraser is a tool; erasing whole strokes
    // takes each one it touches.
    void theEraserToolErasesWholeStrokes() {
        Bench bench;
        bench.stroke(devices().plainPen, QPointF(100, 100), QPointF(500, 100));
        bench.stroke(devices().plainPen, QPointF(100, 300), QPointF(500, 300));
        QCOMPARE(bench.surface->count(), 2);
        bench.surface->setTool(InkSurface::Tool::Eraser);
        bench.surface->setEraser(InkSurface::EraserKind::Strokes);
        bench.stroke(devices().plainPen, QPointF(250, 80), QPointF(260, 120));
        QCOMPARE(bench.surface->count(), 1);
        QCOMPARE(bench.surface->sheet().strokes().front().points.front().at.y(), 300.0);
        bench.surface->undo();
        QCOMPARE(bench.surface->count(), 2);
        bench.surface->undo();
        bench.surface->undo();
        QCOMPARE(bench.surface->count(), 0);
        QVERIFY(!bench.surface->canUndo() && bench.surface->canRedo());
    }

    // With proximity: a touch while the pen is near, or just left, is a
    // palm, ignored and passed to nothing, even where fingers draw.
    void aPalmIsIgnoredWhileThePenIsNear() {
        Bench bench;
        bench.surface->setFingers(InkSurface::FingerUse::Draw);
        bench.proximity(devices().pen, true);
        QVERIFY(bench.surface->penNear());
        bench.swipe(QPointF(100, 300), QPointF(300, 350), 9);
        QCOMPARE(bench.surface->count(), 0);
        QCOMPARE(bench.presses(), 0);
        bench.proximity(devices().pen, false);
        bench.swipe(QPointF(100, 300), QPointF(300, 350), 9);
        QCOMPARE(bench.surface->count(), 0);
        // Well after it left, a finger draws, as asked.
        bench.clock += 2000;
        bench.swipe(QPointF(100, 300), QPointF(300, 350), 9);
        QCOMPARE(bench.surface->count(), 1);
        QCOMPARE(bench.presses(), 0);
    }

    // Without proximity: a pen seen, a finger scrolls (passes the touch on),
    // and just after the pen wrote it is a palm.
    void onlyThePenDrawsOnceOneIsSeen() {
        Bench bench;
        // No pen yet: a finger draws.
        bench.swipe(QPointF(100, 100), QPointF(300, 120), 9);
        QCOMPARE(bench.surface->count(), 1);
        bench.stroke(devices().pen, QPointF(100, 200), QPointF(300, 200));
        QCOMPARE(bench.surface->count(), 2);
        QVERIFY(!bench.surface->rules().proximityKnown());
        // Right after writing: a palm, neither drawn nor passed on.
        bench.swipe(QPointF(100, 300), QPointF(300, 320), 9);
        QCOMPARE(bench.surface->count(), 2);
        QCOMPARE(bench.presses(), 0);
        // Later: passed on to what is under the surface, to scroll it.
        bench.clock += 2000;
        bench.swipe(QPointF(100, 300), QPointF(300, 320), 9);
        QCOMPARE(bench.surface->count(), 2);
        QCOMPARE(bench.presses(), 1);
    }

    // By contact size alone: a touch as wide as a palm never draws, nor
    // one that lands as a fingertip and spreads.
    void aPalmIsToldByItsSize() {
        Bench bench;
        const double mmPerPixel = bench.view().screen()->physicalSize().width() / bench.view().screen()->geometry().width();
        QVERIFY(mmPerPixel > 0.0);
        const double fingertip = 7.0 / mmPerPixel;
        const double palm = 40.0 / mmPerPixel;
        bench.swipe(QPointF(100, 100), QPointF(300, 120), fingertip);
        QCOMPARE(bench.surface->count(), 1);
        bench.swipe(QPointF(100, 200), QPointF(300, 220), palm);
        QCOMPARE(bench.surface->count(), 1);
        bench.swipe(QPointF(100, 300), QPointF(300, 320), fingertip, palm);
        QCOMPARE(bench.surface->count(), 1);
        QCOMPARE(bench.presses(), 0);
    }

    // A palm that came down first, drawing, is no stroke once the pen does.
    void aStrokeOfThePalmGoesWhenThePenComes() {
        Bench bench;
        bench.touch(QEventPoint::State::Pressed, QPointF(400, 400), 9);
        bench.touch(QEventPoint::State::Updated, QPointF(420, 410), 9);
        QVERIFY(bench.surface->writing());
        bench.stroke(devices().pen, QPointF(100, 100), QPointF(300, 100));
        bench.touch(QEventPoint::State::Updated, QPointF(440, 420), 9);
        bench.touch(QEventPoint::State::Released, QPointF(440, 420), 9);
        QCOMPARE(bench.surface->count(), 1);
        QVERIFY(bench.surface->sheet().strokes().front().pressured);
    }

    // Always or never, as the surface says, whether a pen was seen or not.
    void fingersDrawOrScrollAsAsked() {
        Bench bench;
        bench.surface->setFingers(InkSurface::FingerUse::Scroll);
        bench.swipe(QPointF(100, 100), QPointF(300, 120), 9);
        QCOMPARE(bench.surface->count(), 0);
        QCOMPARE(bench.presses(), 1);
        bench.stroke(devices().pen, QPointF(100, 200), QPointF(300, 200));
        bench.clock += 2000;
        bench.surface->setFingers(InkSurface::FingerUse::Draw);
        bench.swipe(QPointF(100, 300), QPointF(300, 320), 9);
        QCOMPARE(bench.surface->count(), 2);
        QCOMPARE(bench.presses(), 1);
    }

    // Where fingers draw, two of them don't (#134): what the first began
    // goes, and together they move the Flickable the surface is on, and say
    // how far they spread, for what zooms. After, one finger writes again.
    void twoFingersMoveWhatIsUnder() {
        using State = QEventPoint::State;
        Bench bench(QString::fromUtf8(scrolled));
        const QQuickItem *under = bench.view().rootObject();
        QSignalSpy pinched(bench.surface, &InkSurface::pinched);
        bench.touch(State::Pressed, QPointF(200, 200), 9);
        bench.touch(State::Updated, QPointF(210, 210), 9);
        QVERIFY(bench.surface->writing());
        bench.touches({bench.finger(State::Stationary, QPointF(210, 210), 9), bench.finger(State::Pressed, QPointF(400, 210), 9, 2)});
        QVERIFY(!bench.surface->writing());
        QVERIFY(bench.surface->touched());
        for (int step = 1; step <= 10; ++step)
            bench.touches({bench.finger(State::Updated, QPointF(210, 210 - (step * 10)), 9),
                           bench.finger(State::Updated, QPointF(400, 210 - (step * 10)), 9, 2)});
        // The window delivers the moves of a touch at its next frame, together.
        QTRY_VERIFY2(std::abs(under->property("contentY").toDouble() - 600) < 0.5, qPrintable(under->property("contentY").toString()));
        QVERIFY(pinched.isEmpty());
        for (int step = 1; step <= 5; ++step)
            bench.touches({bench.finger(State::Updated, QPointF(210 - (step * 10), 110), 9),
                           bench.finger(State::Updated, QPointF(400 + (step * 10), 110), 9, 2)});
        const auto spread = [&pinched] {
            double scale = 1.0;
            for (const QList<QVariant> &one : std::as_const(pinched))
                scale *= one.first().toDouble();
            return scale;
        };
        QTRY_VERIFY2(std::abs(spread() - (290.0 / 190.0)) < 0.01, qPrintable(QString::number(spread())));
        QVERIFY(std::abs(under->property("contentY").toDouble() - 600) < 0.5);
        bench.touches({bench.finger(State::Released, QPointF(160, 110), 9), bench.finger(State::Updated, QPointF(450, 110), 9, 2)});
        bench.touches({bench.finger(State::Released, QPointF(450, 110), 9, 2)});
        QVERIFY(!bench.surface->touched());
        QCOMPARE(bench.surface->count(), 0);
        QVERIFY(bench.pause().isEmpty());
        bench.swipe(QPointF(100, 300), QPointF(300, 320), 9);
        QCOMPARE(bench.surface->count(), 1);
        QVERIFY2(std::abs(under->property("contentY").toDouble() - 600) < 0.5, qPrintable(under->property("contentY").toString()));
    }

    // A mouse draws, unless the surface says it doesn't.
    void aMouseDraws() {
        Bench bench;
        QTest::mousePress(&bench.view(), Qt::LeftButton, {}, QPoint(100, 100));
        for (int x = 110; x <= 300; x += 10)
            QTest::mouseMove(&bench.view(), QPoint(x, 100 + x / 10));
        QTest::mouseRelease(&bench.view(), Qt::LeftButton, {}, QPoint(300, 130));
        QCOMPARE(bench.surface->count(), 1);
        QVERIFY(!bench.surface->sheet().strokes().front().pressured);
        bench.surface->setMouseDraws(false);
        QTest::mousePress(&bench.view(), Qt::LeftButton, {}, QPoint(100, 300));
        QTest::mouseRelease(&bench.view(), Qt::LeftButton, {}, QPoint(200, 300));
        QCOMPARE(bench.surface->count(), 1);
    }

    // What one surface sent, taken by another, draws the same strokes; and
    // all of them at once, as the change log's set or a snapshot.
    void batchesDrawTheSameStrokesElsewhere() {
        Bench here;
        Bench there;
        here.stroke(devices().pen, QPointF(100, 100), QPointF(400, 150), 0.2, 0.8);
        here.stroke(devices().pen, QPointF(100, 200), QPointF(400, 250), 0.9, 0.3);
        here.stroke(devices().eraser, QPointF(250, 50), QPointF(250, 300));
        for (const QVariantMap &batch : here.pause())
            QVERIFY(there.surface->apply(batch));
        const QVariantMap strokes = here.surface->strokes();
        QCOMPARE(strokes.size(), 4);
        QCOMPARE(there.surface->strokes(), strokes);
        Bench again;
        QCOMPARE(again.surface->load(strokes), 4);
        QCOMPARE(again.surface->strokes(), strokes);
        QVERIFY(!again.surface->canUndo());
        Bench restored;
        QVERIFY(restored.surface->restore(here.surface->snapshot()));
        QCOMPARE(restored.surface->strokes(), strokes);
        // A batch with a stroke that can't be read is not taken.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"a batch not taken"_s));
        QVERIFY(!restored.surface->apply({{u"add"_s, QVariantMap{{u"x"_s, u"AAAA"_s}}}}));
        QCOMPARE(restored.surface->count(), 4);
    }

    // A stand-in notebook plugin: its panel is an ink surface that keeps
    // each batch with omabook.log.add, and draws the page back from the
    // log when it opens.
    void aPluginKeepsItsInkInTheChangeLog() {
        QTemporaryDir dir;
        Library library(dir.filePath(u"Library"_s));
        QVERIFY(library.open());
        const QString folder = dir.filePath(u"plugins/notebook"_s);
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(Fixtures::write(folder + "/plugin.json", R"({"name": "Notebook", "version": "1.0", "script": "main.js"})"));
        QVERIFY(Fixtures::write(folder + "/main.js", R"(
omabook.ui.panel({place: "library.side", title: "Notebook", source: "Panel.qml"});
function draw(page, strokes, erased) { omabook.log.add("page", page, "strokes", strokes, erased); return true; }
function page(id) { return omabook.log.read("page", id); }
)"));
        const QByteArray panel = R"(import QtQuick
import Omabook

InkSurface {
    objectName: "ink"
    width: 640
    height: 480
    batchPause: 20
    onInked: (batch) => scripts.call("notebook", "draw", ["p1", batch.add, batch.remove])
    Component.onCompleted: {
        const kept = scripts.call("notebook", "page", ["p1"]);
        if (kept && kept.sets && kept.sets.strokes)
            load(kept.sets.strokes);
    }
}
)";
        QVERIFY(Fixtures::write(folder + "/Panel.qml", panel));
        ScriptServices services;
        services.library = [&library] { return &library; };
        ScriptPlugins plugins(services, dir.filePath(u"plugins"_s), nullptr, false);
        QVERIFY(plugins.plugin(u"notebook"_s) != nullptr);

        const auto open = [&](QQuickView &view) -> InkSurface * {
            view.rootContext()->setContextProperty(u"scripts"_s, &plugins);
            view.setSource(QUrl::fromLocalFile(folder + "/Panel.qml"));
            view.show();
            if (!QTest::qWaitForWindowExposed(&view))
                return nullptr;
            auto *surface = qobject_cast<InkSurface *>(view.rootObject());
            if (surface != nullptr)
                surface->rules() = Ink::InputRules{};
            return surface;
        };
        registerInkTypes();
        QQuickView first;
        InkSurface *written = open(first);
        QVERIFY(written != nullptr);
        // The panel's own window, driven like the bench's.
        ulong clock = 500'000;
        const auto penStroke = [&](const QPointingDevice &device, QPointF from, QPointF to) {
            for (int i = 0; i <= 20; ++i) {
                clock += 4;
                const QPointF at = from + (to - from) * (i / 20.0);
                QWindowSystemInterface::handleTabletEvent(&first, clock, &device, at, first.mapToGlobal(at), Qt::LeftButton, 0.3 + i / 40.0,
                                                          0, 0, 0, 0, 0);
            }
            clock += 4;
            QWindowSystemInterface::handleTabletEvent(&first, clock, &device, to, first.mapToGlobal(to), Qt::NoButton, 0, 0, 0, 0, 0, 0);
            QWindowSystemInterface::flushWindowSystemEvents();
        };
        penStroke(devices().pen, QPointF(50, 100), QPointF(550, 100));
        penStroke(devices().pen, QPointF(50, 200), QPointF(550, 220));
        penStroke(devices().eraser, QPointF(300, 50), QPointF(300, 300));
        QCOMPARE(written->count(), 4);
        QTRY_COMPARE(PluginData::read(library, {.plugin = u"notebook"_s, .entity = u"page"_s, .id = u"p1"_s})
                         .toObject()
                         .value(u"sets"_s)
                         .toObject()
                         .value(u"strokes"_s)
                         .toObject()
                         .size(),
                     4);
        QQuickView second;
        InkSurface *read = open(second);
        QVERIFY(read != nullptr);
        QCOMPARE(read->strokes(), written->strokes());
    }

    // A thousand strokes of handwriting, sixty points each, kept as ink
    // batches of twenty strokes: the log they make has a bounded size.
    void aThousandStrokesMakeALogOfBoundedSize() {
        QTemporaryDir dir;
        Library library(dir.filePath(u"Library"_s));
        QVERIFY(library.open());
        const auto valueBytes = [&] {
            QSqlQuery query = ChangeLog::query(library,
                                               u"SELECT COALESCE(SUM(LENGTH(value) + COALESCE(LENGTH(signature), 0)), 0) FROM space_ops "
                                               "WHERE space = (SELECT id FROM log_ids WHERE name = ?)"_s,
                                               {PluginData::spaceOf(u"notebook")});
            return query.next() ? query.value(0).toLongLong() : -1;
        };
        const qint64 before = valueBytes();
        for (int i = 0; i < 1000; i += 20) {
            Ink::Change change;
            for (int j = i; j < i + 20; ++j)
                change.added.push_back(handwriting(QPointF(10, j), 60, j));
            for (const QJsonObject &batch : Ink::batches(change)) {
                const auto kept = PluginData::change(library, {.plugin = u"notebook"_s, .entity = u"page"_s, .id = u"p2"_s}, u"strokes"_s,
                                                     {.added = batch.value(u"add"_s).toObject(), .removed = {}});
                QVERIFY2(kept.has_value(), qPrintable(whyNot(kept)));
            }
        }
        const qint64 perThousand = valueBytes() - before;
        qInfo() << "1000 strokes of 60 points in the change log:" << perThousand << "bytes of values and signatures,"
                << perThousand / 60'000.0 << "a point";
        // Four to six bytes a point, in base64 with its key and signature: under 400 KB a thousand strokes.
        QVERIFY2(perThousand < 400'000, qPrintable(QString::number(perThousand)));
        QCOMPARE(PluginData::read(library, {.plugin = u"notebook"_s, .entity = u"page"_s, .id = u"p2"_s})
                     .toObject()
                     .value(u"sets"_s)
                     .toObject()
                     .value(u"strokes"_s)
                     .toObject()
                     .size(),
                 1000);
    }

    // A page of 2,000 strokes: put together once when it opens, then each
    // frame while the pen writes redraws only the line being written.
    void aPageOfTwoThousandStrokesDrawsWithinAFrame() {
        Bench bench;
        std::vector<Ink::Stroke> strokes;
        for (int i = 0; i < 2000; ++i)
            strokes.push_back(handwriting(QPointF(10 + (i % 8) * 78, 10 + (i / 8) * 1.8), 60, i));
        bench.surface->replace(std::move(strokes));
        QVERIFY(bench.frame());
        const double opened = bench.surface->frameTime();
        bench.pen(devices().pen, QPointF(100, 100), 0.5, true);
        double writing = 0.0;
        QElapsedTimer clock;
        double longestFrame = 0.0;
        for (int i = 1; i <= 30; ++i) {
            bench.pen(devices().pen, QPointF(100 + i * 5, 100 + i), 0.5, true);
            clock.start();
            QVERIFY(bench.frame());
            longestFrame = std::max(longestFrame, static_cast<double>(clock.nsecsElapsed()) / 1e6);
            writing = std::max(writing, bench.surface->frameTime());
        }
        bench.pen(devices().pen, QPointF(250, 130), 0.0, false);
        QCOMPARE(bench.surface->count(), 2001);
        qInfo() << "2000 strokes: the first frame put together in" << opened << "ms; while writing, at most" << writing
                << "ms, and a whole frame at most" << longestFrame << "ms";
        // Well within a frame (16 ms), on a machine several times slower than this one.
        QVERIFY2(writing < 4, qPrintable(QString::number(writing)));
        QVERIFY2(opened < 250, qPrintable(QString::number(opened)));
        bench.shot(u"ink-2000-strokes"_s);
    }

    // The lasso (#129): a path drawn with the pen, the mouse or a finger
    // selects the strokes most of whose points it goes around, which is no
    // edit; dragged, they move, one step of undo and one batch, which
    // removes them and adds them again where they went, with new ids.
    void theLassoSelectsAndMovesStrokes() {
        Bench bench;
        bench.surface->setFingers(InkSurface::FingerUse::Draw);
        bench.stroke(devices().pen, QPointF(100, 100), QPointF(300, 100));
        bench.stroke(devices().pen, QPointF(100, 160), QPointF(300, 160));
        bench.stroke(devices().pen, QPointF(100, 400), QPointF(300, 400));
        std::ignore = bench.pause();
        const QStringList top = beginningAt(*bench.surface, QPointF(100, 100)) + beginningAt(*bench.surface, QPointF(100, 160));
        QCOMPARE(top.size(), 2);
        bench.surface->setTool(InkSurface::Tool::Lasso);
        InkSelection *selection = bench.surface->selection();
        QSignalSpy told(selection, &InkSelection::changed);
        bench.penLoop({{80, 80}, {320, 80}, {320, 180}, {80, 180}});
        QCOMPARE(selection->count(), 2);
        QVERIFY(!told.isEmpty());
        QVERIFY(selection->box().contains(QRectF(100, 100, 200, 60)));
        QCOMPARE(bench.surface->count(), 3);
        QVERIFY(bench.pause().isEmpty());
        QVERIFY(bench.frame());
        look(*bench.surface, u"ink-lasso-selected"_s);
        // The mouse and a finger select too: around nothing is nothing.
        bench.mouseLoop({{50, 300}, {350, 300}, {350, 450}, {50, 450}});
        QCOMPARE(selection->count(), 1);
        // Long enough after the pen that a finger is no palm.
        bench.clock += 2000;
        bench.touchLoop({{400, 20}, {600, 20}, {600, 60}, {400, 60}});
        QCOMPARE(selection->count(), 0);
        bench.touchLoop({{80, 80}, {320, 80}, {320, 180}, {80, 180}});
        QCOMPARE(selection->count(), 2);

        // Dragged with the pen from within its box, half way: drawn where it goes, nothing moved yet.
        for (int i = 0; i <= 10; ++i)
            bench.pen(devices().pen, QPointF(200, 130) + QPointF(5, 10) * i, 0.5, true);
        QVERIFY(bench.frame());
        look(*bench.surface, u"ink-lasso-dragged"_s);
        QCOMPARE(beginningAt(*bench.surface, QPointF(100, 100)).size(), 1);
        bench.pen(devices().pen, QPointF(250, 230), 0.0, false);
        QVERIFY(beginningAt(*bench.surface, QPointF(100, 100)).isEmpty());
        const QStringList moved = beginningAt(*bench.surface, QPointF(150, 200)) + beginningAt(*bench.surface, QPointF(150, 260));
        QCOMPARE(moved.size(), 2);
        QCOMPARE(bench.surface->count(), 3);
        QCOMPARE(setOf(selection->lasso().selection()), setOf(moved));
        auto batches = bench.pause();
        QCOMPARE(batches.size(), 1);
        QCOMPARE(setOf(keys(batches.front(), "remove")), setOf(top));
        QCOMPARE(keys(batches.front(), "add").size(), 2);
        QVERIFY(!top.contains(keys(batches.front(), "add").front()));

        // Undone, as one step: the strokes back where they were, and selected.
        bench.surface->undo();
        QCOMPARE(beginningAt(*bench.surface, QPointF(100, 100)).size(), 1);
        QCOMPARE(selection->count(), 2);
        batches = bench.pause();
        QCOMPARE(batches.size(), 1);
        QCOMPARE(keys(batches.front(), "remove").size(), 2);
        QCOMPARE(setOf(keys(batches.front(), "add")), setOf(top));
        bench.surface->redo();
        QCOMPARE(beginningAt(*bench.surface, QPointF(150, 200)).size(), 1);
        // A batch of it, taken elsewhere, draws the same.
        Bench there;
        QVERIFY(there.surface->load(bench.surface->strokes()) == 3);
    }

    // The selection answers to keys while the surface has them, which it
    // takes when the lasso selects, and to its own functions; at any
    // scale, in item pixels. Each is a step of undo.
    void theSelectionAnswersToKeysAtAnyScale() {
        Bench bench;
        bench.view().requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&bench.view()));
        bench.surface->setInkScale(2.0);
        bench.stroke(devices().pen, QPointF(100, 100), QPointF(300, 100));
        bench.stroke(devices().pen, QPointF(100, 300), QPointF(300, 300));
        std::ignore = bench.pause();
        const QString first = beginningAt(*bench.surface, QPointF(50, 50)).front();
        bench.surface->setTool(InkSurface::Tool::Lasso);
        InkSelection *selection = bench.surface->selection();
        bench.mouseLoop({{80, 80}, {320, 80}, {320, 140}, {80, 140}});
        QCOMPARE(selection->count(), 1);
        QVERIFY(bench.surface->hasActiveFocus());
        QVERIFY(selection->box().contains(QRectF(100, 99, 200, 2)));

        // Shift and an arrow, ten pixels: five ink units; then one pixel down.
        QTest::keyClick(&bench.view(), Qt::Key_Right, Qt::ShiftModifier);
        QTest::keyClick(&bench.view(), Qt::Key_Down);
        const QStringList moved = beginningAt(*bench.surface, QPointF(55, 50.5));
        QCOMPARE(moved.size(), 1);
        auto batches = bench.pause();
        QCOMPARE(batches.size(), 1);
        QCOMPARE(keys(batches.front(), "remove"), QStringList{first});
        QCOMPARE(keys(batches.front(), "add"), moved);
        bench.surface->undo();
        bench.surface->undo();
        QCOMPARE(beginningAt(*bench.surface, QPointF(50, 50)), QStringList{first});
        QCOMPARE(selection->lasso().selection(), QStringList{first});
        std::ignore = bench.pause();

        selection->recolor(Qt::red);
        const QString red = beginningAt(*bench.surface, QPointF(50, 50)).front();
        QCOMPARE(bench.surface->sheet().find(red)->color, QColor(Qt::red));
        batches = bench.pause();
        QCOMPARE(batches.size(), 1);
        QCOMPARE(keys(batches.front(), "remove"), QStringList{first});
        QCOMPARE(keys(batches.front(), "add"), QStringList{red});

        // Copied and pasted beside itself, twelve pixels: six ink units; the copy selected.
        QTest::keyClick(&bench.view(), Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(&bench.view(), Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(bench.surface->count(), 3);
        const QStringList copy = beginningAt(*bench.surface, QPointF(56, 56));
        QCOMPARE(copy.size(), 1);
        QCOMPARE(selection->lasso().selection(), copy);
        batches = bench.pause();
        QCOMPARE(batches.size(), 1);
        QCOMPARE(keys(batches.front(), "add"), copy);
        QVERIFY(keys(batches.front(), "remove").isEmpty());
        look(*bench.surface, u"ink-lasso-pasted"_s);

        QTest::keyClick(&bench.view(), Qt::Key_Delete);
        QCOMPARE(bench.surface->count(), 2);
        QCOMPARE(selection->count(), 0);
        batches = bench.pause();
        QCOMPARE(batches.size(), 1);
        QCOMPARE(keys(batches.front(), "remove"), copy);
        QTest::keyClick(&bench.view(), Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(selection->count(), 2);
        QTest::keyClick(&bench.view(), Qt::Key_Escape);
        QCOMPARE(selection->count(), 0);
        bench.surface->undo();
        QCOMPARE(bench.surface->count(), 3);
        // Read only: what is selected stays as it is.
        selection->selectAll();
        bench.surface->setReadOnly(true);
        selection->remove();
        QCOMPARE(bench.surface->count(), 3);
    }

    // A plugin is told where each stroke is and what the lasso selected
    // (#133), in item pixels, from QML, without reading the strokes' bytes.
    void aPluginIsToldWhereStrokesAreAndWhatIsSelected() {
        Bench bench;
        bench.surface->setInkScale(2.0);
        bench.stroke(devices().pen, QPointF(100, 100), QPointF(300, 100));
        bench.stroke(devices().pen, QPointF(100, 300), QPointF(300, 300));
        std::ignore = bench.pause();
        const QString first = beginningAt(*bench.surface, QPointF(50, 50)).front();
        const auto ask = [&bench](const QString &code) {
            QQmlExpression expression(qmlContext(bench.surface), bench.surface, code);
            const QVariant answer = expression.evaluate();
            const QString wrong = expression.hasError() ? expression.error().toString() : QString();
            [&] { QVERIFY2(wrong.isEmpty(), qPrintable(wrong)); }();
            return answer;
        };
        const QVariantMap bounds = ask(u"bounds()"_s).toMap();
        QCOMPARE(bounds.size(), 2);
        const QRectF where = bounds.value(first).toRectF();
        QVERIFY(where.contains(QRectF(100, 99, 200, 2)));
        QVERIFY(!where.contains(QPointF(100, 300)));
        QCOMPARE(ask(u"boundsOf(\"%1\")"_s.arg(first)).toRectF(), where);
        QCOMPARE(ask(u"boundsOf(\"nothing\")"_s).toRectF(), QRectF());

        QSignalSpy told(bench.surface->selection(), &InkSelection::changed);
        bench.surface->setTool(InkSurface::Tool::Lasso);
        bench.mouseLoop({{80, 80}, {320, 80}, {320, 140}, {80, 140}});
        QCOMPARE(ask(u"selection.ids"_s).toStringList(), QStringList{first});
        QCOMPARE(ask(u"selection.box"_s).toRectF(), where);
        // What recognize.ink takes: {id: base64}, the stroke itself.
        const QVariantMap selected = ask(u"selection.strokes()"_s).toMap();
        QCOMPARE(selected.keys(), QStringList{first});
        QCOMPARE(selected.value(first), bench.surface->strokes().value(first));
        ask(u"selection.select(Object.keys(bounds()))"_s);
        QCOMPARE(ask(u"selection.ids.length"_s).toInt(), 2);
        // A stroke recoloured is another: its id changes where the box doesn't, and that is told.
        ask(u"selection.select([\"%1\"])"_s.arg(first));
        const auto before = told.size();
        ask(u"selection.recolor(\"red\")"_s);
        QVERIFY(told.size() > before);
        QVERIFY(!ask(u"selection.ids"_s).toStringList().contains(first));
        QCOMPARE(ask(u"selection.box"_s).toRectF(), where);
    }

    // What is drawn is where the strokes are: dark along a line, light
    // away from it (where the scene graph draws geometry offscreen).
    void theStrokesAreDrawnWhereTheyAre() {
        Bench bench;
        bench.surface->setPenWidth(4.0);
        bench.stroke(devices().pen, QPointF(100, 100), QPointF(500, 100), 0.2, 1.0);
        bench.stroke(devices().pen, QPointF(100, 200), QPointF(500, 300));
        bench.pen(devices().pen, QPointF(320, 400), 0.6, true);
        bench.pen(devices().pen, QPointF(320, 400), 0.0, false);
        QVERIFY(bench.frame());
        bench.shot(u"ink-strokes"_s);
        const QImage image = bench.rendersGeometry() ? bench.grab() : painted(*bench.surface, QSize(640, 480));
        if (const QString folder = qEnvironmentVariable("OMABOOK_SHOTS"); !folder.isEmpty())
            image.save(folder + "/ink-strokes-drawn.png");
        const qreal ratio = image.devicePixelRatio();
        const auto dark = [&](QPointF at) { return qGray(image.pixel((at * ratio).toPoint())) < 128; };
        QVERIFY(dark(QPointF(300, 100)));
        QVERIFY(dark(QPointF(300, 250)));
        QVERIFY(dark(QPointF(320, 400)));
        QVERIFY(!dark(QPointF(300, 130)));
        QVERIFY(!dark(QPointF(300, 50)));
    }
};

QTEST_MAIN(InkSurfaceTests)
#include "inksurface_tests.moc"
