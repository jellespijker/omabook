#include "inksurface.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QInputDevice>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTabletEvent>

#include <algorithm>
#include <tuple>

#include "inkcodec.h"
#include "inknodes.h"

using Ink::can;
using Ink::deviceId;
using Ink::knowsAPen;

namespace {

// The key of a pen's or the mouse's pointer, apart from a touch's point ids.
constexpr int toolPoint = -1;
constexpr int mousePoint = -2;
constexpr int defaultBatchPause = 1000;
// How far outside the selection's box, in item pixels, the lasso still takes it to drag.
constexpr double lassoGrab = 8.0;

} // namespace

InkSurface::InkSurface(QQuickItem *parent)
    : QQuickItem(parent), m_selection(m_lasso, *this, [this](Ink::Change change) { edited(std::move(change)); }), m_rules(knowsAPen()),
      m_nodes(std::make_unique<InkNodes>()) {
    setFlag(ItemHasContents);
    setAcceptTouchEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    // What this surface draws with is its own: a list under it doesn't take it to scroll.
    setKeepTouchGrab(true);
    setKeepMouseGrab(true);
    m_batchTimer.setSingleShot(true);
    m_batchTimer.setInterval(defaultBatchPause);
    connect(&m_batchTimer, &QTimer::timeout, this, [this] {
        if (m_writing.empty()) {
            flush();
        }
    });
    // A pen's events come to the window before any item, and its proximity
    // to the application alone, so the surface hears them there.
    QCoreApplication::instance()->installEventFilter(this);
}

InkSurface::~InkSurface() = default;

void InkSurface::setColor(const QColor &color) {
    if (m_color != color) {
        m_color = color;
        emit lookChanged();
    }
}

void InkSurface::setPenWidth(double width) {
    if (width > 0.0 && m_penWidth != width) {
        m_penWidth = width;
        emit lookChanged();
    }
}

void InkSurface::setEraserWidth(double width) {
    if (width > 0.0 && m_eraserWidth != width) {
        m_eraserWidth = width;
        emit lookChanged();
    }
}

void InkSurface::setInkScale(double scale) {
    if (scale > 0.0 && m_inkScale != scale) {
        m_inkScale = scale;
        m_selection.setScale(scale);
        m_allDirty = true;
        update();
        emit lookChanged();
    }
}

void InkSurface::setTool(Tool tool) {
    if (m_tool != tool) {
        m_tool = tool;
        setActiveFocusOnTab(tool == Tool::Lasso);
        emit toolChanged();
    }
}

void InkSurface::setEraser(EraserKind kind) {
    if (m_eraser != kind) {
        m_eraser = kind;
        emit toolChanged();
    }
}

void InkSurface::setFingers(FingerUse use) {
    if (m_fingers != use) {
        m_fingers = use;
        emit toolChanged();
    }
}

void InkSurface::setMouseDraws(bool draws) {
    if (m_mouseDraws != draws) {
        m_mouseDraws = draws;
        emit toolChanged();
    }
}

void InkSurface::setReadOnly(bool readOnly) {
    if (m_readOnly != readOnly) {
        m_readOnly = readOnly;
        m_selection.setReadOnly(readOnly);
        dropTouches();
        emit toolChanged();
    }
}

void InkSurface::setBatchPause(int milliseconds) {
    if (milliseconds >= 0 && m_batchTimer.interval() != milliseconds) {
        m_batchTimer.setInterval(milliseconds);
        emit toolChanged();
    }
}

void InkSurface::undo() { edited(m_sheet.undo()); }

void InkSurface::redo() { edited(m_sheet.redo()); }

void InkSurface::clear() { edited(m_sheet.clear()); }

void InkSurface::flush() {
    m_batchTimer.stop();
    if (m_pending.empty()) {
        return;
    }
    const std::vector<QJsonObject> batches = Ink::batches(m_pending);
    m_pending = {};
    for (const QJsonObject &batch : batches) {
        emit inked(batch.toVariantMap());
    }
}

QVariantMap InkSurface::strokes() const { return Ink::items(m_sheet.strokes()).toVariantMap(); }

int InkSurface::load(const QVariantMap &strokes) {
    Ink::Read read = Ink::fromItems(QJsonObject::fromVariantMap(strokes));
    for (const QString &problem : std::as_const(read.problems)) {
        qWarning("Ink: left out: %s", qPrintable(problem));
    }
    const auto count = static_cast<int>(read.strokes.size());
    replace(std::move(read.strokes));
    return count;
}

bool InkSurface::apply(const QVariantMap &batch) {
    const auto change = Ink::fromBatch(QJsonObject::fromVariantMap(batch));
    if (!change) {
        qWarning("Ink: a batch not taken: %s", qPrintable(change.error()));
        return false;
    }
    applyChange(*change);
    return true;
}

QString InkSurface::snapshot() const { return QString::fromLatin1(Ink::encodeAll(m_sheet.strokes()).toBase64()); }

bool InkSurface::restore(const QString &snapshot) {
    auto strokes = Ink::decodeAll(QByteArray::fromBase64(snapshot.toLatin1()));
    if (!strokes) {
        qWarning("Ink: a snapshot not taken: %s", qPrintable(strokes.error()));
        return false;
    }
    replace(std::move(*strokes));
    return true;
}

void InkSurface::replace(std::vector<Ink::Stroke> strokes) {
    // What was done here and not yet sent goes first, so it isn't lost.
    flush();
    m_sheet.replace(std::move(strokes));
    m_lasso.prune();
    m_restructured = true;
    m_allDirty = true;
    emit strokesChanged();
    update();
}

void InkSurface::applyChange(const Ink::Change &change) {
    m_sheet.apply(change);
    m_lasso.prune();
    for (const Ink::Stroke &stroke : change.added) {
        m_dirty.insert(stroke.id);
    }
    m_restructured = true;
    emit strokesChanged();
    update();
}

QSGNode *InkSurface::updatePaintNode(QSGNode *old, [[maybe_unused]] UpdatePaintNodeData *data) {
    QElapsedTimer clock;
    clock.start();
    const bool fresh = old == nullptr;
    QSGNode *root = m_nodes->root(old);
    const std::vector<const Ink::Stroke *> order = InkNodes::shown(m_sheet, m_writing, m_lasso, m_dirty);
    const double pixelRatio = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
    m_nodes->sync(order, m_dirty, m_restructured || fresh, m_allDirty || fresh,
                  InkNodes::Look{.scale = m_inkScale, .feather = 1.0 / pixelRatio});
    Ink::Mesh marks;
    m_selection.marks(marks);
    m_nodes->marks(marks);
    m_dirty.clear();
    m_restructured = false;
    m_allDirty = false;
    m_frameTime = static_cast<double>(clock.nsecsElapsed()) / 1e6;
    return root;
}

bool InkSurface::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::TabletEnterProximity:
    case QEvent::TabletLeaveProximity: {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the event's type says its class, Qt's own idiom
        const auto *proximity = static_cast<const QTabletEvent *>(event);
        m_rules.proximity(event->type() == QEvent::TabletEnterProximity, proximity->timestamp());
        emit penChanged();
        return false;
    }
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        // The application's filter sees the event once for each object it
        // goes to; the window's copy is the one that counts.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the event's type says its class, Qt's own idiom
        return watched == window() && window() != nullptr && tablet(*static_cast<QTabletEvent *>(event));
    default:
        return false;
    }
}

bool InkSurface::tablet(QTabletEvent &event) {
    const QPointingDevice *device = event.pointingDevice();
    const bool seen = m_rules.penSeen();
    m_rules.penEvent(event.timestamp());
    if (!seen) {
        emit penChanged();
    }
    const Ink::Pointer pointer{deviceId(device), toolPoint};
    const QPointF at = mapFromScene(event.scenePosition());
    Ink::Input input = inputAt(at, event.timestamp());
    input.pressure = static_cast<float>(event.pressure());
    input.xTilt = static_cast<float>(event.xTilt());
    input.yTilt = static_cast<float>(event.yTilt());
    if (event.type() == QEvent::TabletPress) {
        if (m_readOnly || !isVisible() || !isEnabled() || !contains(at)) {
            return false;
        }
        // A palm that came down before the pen did is no stroke.
        dropTouches();
        const bool eraserEnd = device != nullptr && device->pointerType() == QPointingDevice::PointerType::Eraser;
        begin(pointer, input, eraserEnd || m_tool == Tool::Eraser, false, device);
    } else if (!m_writing.has(pointer)) {
        // The pen hovering, or down elsewhere.
        return false;
    } else if (event.type() == QEvent::TabletRelease) {
        lift(pointer, input);
    } else {
        edited(m_writing.move(pointer, input));
        update();
    }
    // Taken: Qt makes no mouse event of it.
    event.accept();
    return true;
}

void InkSurface::mousePressEvent(QMouseEvent *event) {
    // Only a mouse's own: Qt makes mouse events of a pen and a finger too,
    // and those the surface has had as what they are.
    const QPointingDevice *device = event->pointingDevice();
    if (m_readOnly || !m_mouseDraws || event->button() != Qt::LeftButton || device == nullptr
        || device->type() != QInputDevice::DeviceType::Mouse || event->source() != Qt::MouseEventNotSynthesized) {
        event->ignore();
        return;
    }
    begin(Ink::Pointer{deviceId(device), mousePoint}, inputAt(event->position(), event->timestamp()), m_tool == Tool::Eraser, false);
    event->accept();
}

void InkSurface::mouseMoveEvent(QMouseEvent *event) {
    const Ink::Pointer pointer{deviceId(event->pointingDevice()), mousePoint};
    if (!m_writing.has(pointer)) {
        event->ignore();
        return;
    }
    edited(m_writing.move(pointer, inputAt(event->position(), event->timestamp())));
    update();
}

void InkSurface::mouseReleaseEvent(QMouseEvent *event) {
    const Ink::Pointer pointer{deviceId(event->pointingDevice()), mousePoint};
    if (!m_writing.has(pointer)) {
        event->ignore();
        return;
    }
    lift(pointer, inputAt(event->position(), event->timestamp()));
}

void InkSurface::itemChange(ItemChange change, const ItemChangeData &value) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): ItemChangeData is Qt's union, read by the change it is of
    if ((change == ItemVisibleHasChanged && !value.boolValue) || (change == ItemSceneChange && value.window == nullptr)) {
        dropTouches();
        flush();
    }
    if (change == ItemDevicePixelRatioHasChanged) {
        m_allDirty = true;
        update();
    }
    QQuickItem::itemChange(change, value);
}

void InkSurface::begin(Ink::Pointer pointer, const Ink::Input &input, bool erasing, bool touch, const QPointingDevice *device) {
    const bool first = m_writing.empty();
    if (erasing) {
        const auto how = m_eraser == EraserKind::Strokes ? Ink::Erasing::Strokes : Ink::Erasing::Parts;
        edited(m_writing.erase(pointer, input, Ink::Rubber{.radius = m_eraserWidth / 2.0 / m_inkScale, .how = how}, touch));
    } else if (m_tool == Tool::Lasso) {
        std::ignore = m_writing.lasso(pointer, input, lassoGrab / m_inkScale, touch);
    } else {
        m_writing.write(pointer, input,
                        Ink::Nib{.color = m_color,
                                 .width = m_penWidth,
                                 .pressured = can(device, QInputDevice::Capability::Pressure),
                                 .tilted = can(device, QInputDevice::Capability::XTilt)},
                        touch);
    }
    m_batchTimer.stop();
    if (first) {
        emit writingChanged();
    }
    update();
}

void InkSurface::lift(Ink::Pointer pointer, const Ink::Input &input) {
    edited(m_writing.lift(pointer, input));
    if (m_writing.empty()) {
        m_batchTimer.start();
        emit writingChanged();
    }
    update();
}

void InkSurface::drop(Ink::Pointer pointer) {
    m_writing.drop(pointer);
    m_restructured = true;
    if (m_writing.empty()) {
        m_batchTimer.start();
        emit writingChanged();
    }
    update();
}

void InkSurface::dropTouches() {
    m_gesture.clear();
    // Neither the pen nor the mouse is cut short; they lift as they will.
    const std::vector<Ink::Pointer> dropped = m_writing.dropTouches();
    if (dropped.empty()) {
        return;
    }
    m_palms.unite(QSet<Ink::Pointer>(dropped.begin(), dropped.end()));
    m_restructured = true;
    if (m_writing.empty()) {
        emit writingChanged();
    }
    update();
}

Ink::Input InkSurface::inputAt(QPointF inItem, quint64 ms) const { return Ink::Input{.at = inItem / m_inkScale, .ms = ms}; }

void InkSurface::edited(Ink::Change change) {
    if (change.empty()) {
        return;
    }
    // One stroke on top of all the others is a node more; anything else moves them.
    const bool appended = change.removed.isEmpty() && change.added.size() == 1 && !m_sheet.strokes().empty()
                          && m_sheet.strokes().back().id == change.added.front().id;
    m_restructured = m_restructured || !appended || !m_writing.empty();
    m_lasso.follow(change);
    for (const Ink::Stroke &stroke : change.added) {
        m_dirty.insert(stroke.id);
    }
    m_pending.merge(std::move(change));
    if (m_writing.empty()) {
        m_batchTimer.start();
    }
    emit strokesChanged();
    update();
}

double InkSurface::millimetresPerPixel() const {
    const QScreen *screen = window() != nullptr ? window()->screen() : nullptr;
    if (screen == nullptr || screen->geometry().width() <= 0) {
        return 0.0;
    }
    return screen->physicalSize().width() / screen->geometry().width();
}

void registerInkTypes() {
    // Once, however many times it is asked.
    [[maybe_unused]] static const bool registered = [] {
        qmlRegisterType<InkSurface>("Omabook", 1, 0, "InkSurface");
        qmlRegisterUncreatableType<InkSelection>("Omabook", 1, 0, "InkSelection", QString::fromLatin1("an ink surface's own"));
        return true;
    }();
}
