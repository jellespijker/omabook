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
    void touch(QEventPoint::State state, QPointF at, double diameter, int id = 1) {
        clock += 8;
        QWindowSystemInterface::TouchPoint point;
        point.id = id;
        point.state = state;
        point.pressure = state == QEventPoint::State::Released ? 0.0 : 1.0;
        const QPointF global = m_view.mapToGlobal(at);
        point.area = QRectF(0, 0, diameter, diameter);
        point.area.moveCenter(global);
        const QRect screen = m_view.screen()->geometry();
        point.normalPosition = QPointF(global.x() / screen.width(), global.y() / screen.height());
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(&m_view, clock, devices().finger, {point});
    }
    void swipe(QPointF from, QPointF to, double diameter, double grownTo = -1) {
        touch(QEventPoint::State::Pressed, from, diameter);
        for (int i = 1; i <= 10; ++i)
            touch(QEventPoint::State::Updated, from + (to - from) * (i / 10.0), grownTo > 0 && i > 3 ? grownTo : diameter);
        touch(QEventPoint::State::Released, to, diameter);
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

// The surface's strokes as their triangles say, painted: what the scene
// graph would draw, where it draws no geometry (the software renderer).
QImage painted(const InkSurface &surface, QSize size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    Ink::Mesh mesh;
    for (const Ink::Stroke &stroke : surface.sheet().strokes()) {
        Ink::tessellate(stroke, surface.inkScale(), 1.0, mesh);
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
    return image;
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
