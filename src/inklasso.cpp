#include "inklasso.h"

#include <QDateTime>
#include <QLineF>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Ink {

namespace {

// A path's points are kept this far apart at least: enough to follow any hand.
constexpr double pathStep = 0.5;
// The most line pieces the path is shown with, well inside a mesh's 16-bit indices.
constexpr qsizetype mostPieces = 8000;
// How strongly the box around the selection is filled.
constexpr double boxFill = 0.12;
// How far the box stands off the strokes, in its lines' widths.
constexpr double boxClearance = 3.0;

void translate(Stroke &stroke, QPointF by) {
    for (Point &point : stroke.points) {
        point.at += by;
    }
}

QSet<QString> setOf(const QStringList &ids) { return {ids.begin(), ids.end()}; }

Vertex vertex(QPointF at, QColor color, double alpha) {
    const double a = color.alphaF() * alpha;
    const auto channel = [a](double value) { return static_cast<quint8>(std::lround(value * a * 255.0)); };
    return Vertex{.x = static_cast<float>(at.x()),
                  .y = static_cast<float>(at.y()),
                  .r = channel(color.redF()),
                  .g = channel(color.greenF()),
                  .b = channel(color.blueF()),
                  .a = static_cast<quint8>(std::lround(a * 255.0))};
}

// Four corners, two triangles.
void quad(Mesh &mesh, const std::array<QPointF, 4> &corners, QColor color, double alpha) {
    const auto first = static_cast<quint16>(mesh.vertices.size());
    for (const QPointF &corner : corners) {
        mesh.vertices.push_back(vertex(corner, color, alpha));
    }
    for (const int i : {0, 1, 2, 0, 2, 3}) {
        mesh.indices.push_back(static_cast<quint16>(first + i));
    }
}

// A straight piece of line, its ends squared off half its width beyond the points, so that pieces meet.
void piece(Mesh &mesh, QPointF from, QPointF to, double width, QColor color) {
    const QLineF line(from, to);
    if (line.length() <= 0.0) {
        return;
    }
    const QPointF along = (to - from) / line.length() * (width / 2.0);
    const QPointF across(-along.y(), along.x());
    quad(mesh, {from - along + across, to + along + across, to + along - across, from - along - across}, color, 1.0);
}

} // namespace

bool lassoed(const Stroke &stroke, const QPolygonF &path) {
    if (path.size() < 3 || stroke.points.empty()) {
        return false;
    }
    const auto inside =
        std::ranges::count_if(stroke.points, [&](const Point &point) { return path.containsPoint(point.at, Qt::OddEvenFill); });
    return 2 * static_cast<std::size_t>(inside) > stroke.points.size();
}

void Lasso::press(QPointF at, double grab) {
    cancel();
    const QRectF reach = box().adjusted(-grab, -grab, grab, grab);
    if (!m_selection.isEmpty() && reach.contains(at)) {
        m_gesture = Gesture::Drag;
        m_from = at;
        m_offset = {};
        m_moving = selected();
    } else {
        m_gesture = Gesture::Path;
        m_path = QPolygonF{at};
    }
    changed();
}

void Lasso::drag(QPointF to) {
    if (m_gesture == Gesture::Path) {
        if (QLineF(m_path.last(), to).length() >= pathStep) {
            m_path.append(to);
            changed();
        }
    } else if (m_gesture == Gesture::Drag && to - m_from != m_offset) {
        const QPointF by = to - m_from - m_offset;
        m_offset = to - m_from;
        for (Stroke &stroke : m_moving) {
            translate(stroke, by);
        }
        changed();
    }
}

Change Lasso::release(QPointF at) {
    drag(at);
    const Gesture gesture = std::exchange(m_gesture, Gesture::None);
    if (gesture == Gesture::Path) {
        const QPolygonF path = std::exchange(m_path, {});
        const QRectF around = path.boundingRect();
        QStringList ids;
        for (const Stroke &stroke : m_sheet->strokes()) {
            if (bounds(stroke).intersects(around) && lassoed(stroke, path)) {
                ids.append(stroke.id);
            }
        }
        // A tap, or a path around nothing, leaves nothing selected.
        select(ids);
        return {};
    }
    if (gesture != Gesture::Drag) {
        return {};
    }
    for (const Stroke &stroke : std::exchange(m_moving, {})) {
        m_stale.append(stroke.id);
    }
    const QPointF by = std::exchange(m_offset, {});
    Change change = move(by);
    changed();
    return change;
}

void Lasso::cancel() {
    if (m_gesture == Gesture::None) {
        return;
    }
    for (const Stroke &stroke : std::exchange(m_moving, {})) {
        m_stale.append(stroke.id);
    }
    m_gesture = Gesture::None;
    m_path.clear();
    m_offset = {};
    changed();
}

QRectF Lasso::box() const {
    QRectF around;
    const QSet<QString> chosen = setOf(m_selection);
    for (const Stroke &stroke : m_sheet->strokes()) {
        if (chosen.contains(stroke.id)) {
            around = around.isNull() ? bounds(stroke) : around.united(bounds(stroke));
        }
    }
    return around.translated(m_offset);
}

const Stroke *Lasso::moving(QStringView id) const {
    if (m_moving.empty()) {
        return nullptr;
    }
    const auto found = std::ranges::find(m_moving, id, &Stroke::id);
    return found != m_moving.end() ? &*found : nullptr;
}

QStringList Lasso::takeStale() { return std::exchange(m_stale, {}); }

std::vector<Stroke> Lasso::selected() const {
    std::vector<Stroke> out;
    const QSet<QString> chosen = setOf(m_selection);
    for (const Stroke &stroke : m_sheet->strokes()) {
        if (chosen.contains(stroke.id)) {
            out.push_back(stroke);
        }
    }
    return out;
}

void Lasso::select(QStringList ids) {
    ids.removeIf([this](const QString &id) { return m_sheet->find(id) == nullptr; });
    ids.removeDuplicates();
    m_selection = std::move(ids);
    changed();
}

void Lasso::selectAll() {
    QStringList ids;
    for (const Stroke &stroke : m_sheet->strokes()) {
        ids.append(stroke.id);
    }
    select(std::move(ids));
}

void Lasso::deselect() {
    if (!m_selection.isEmpty()) {
        m_selection.clear();
        changed();
    }
}

Change Lasso::move(QPointF by) {
    if (m_selection.isEmpty() || by.isNull()) {
        return {};
    }
    Change change = m_sheet->rework(m_selection, [by](Stroke &stroke) { translate(stroke, by); });
    follow(change);
    return change;
}

Change Lasso::recolor(const QColor &color) {
    QStringList other;
    for (const Stroke &stroke : selected()) {
        if (stroke.color != color) {
            other.append(stroke.id);
        }
    }
    if (other.isEmpty() || !color.isValid()) {
        return {};
    }
    Change change = m_sheet->rework(other, [&color](Stroke &stroke) { stroke.color = color; });
    follow(change);
    return change;
}

Change Lasso::remove() {
    Change change = m_sheet->remove(m_selection);
    follow(change);
    return change;
}

Change Lasso::paste(std::vector<Stroke> strokes, QPointF by) {
    // On top of what is there, one over the other as they were.
    std::ranges::stable_sort(strokes, {}, &Stroke::began);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList ids;
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        Stroke &stroke = strokes[i];
        stroke.id = newId();
        stroke.began = now + static_cast<qint64>(i);
        translate(stroke, by);
        ids.append(stroke.id);
    }
    Change change = m_sheet->addAll(std::move(strokes));
    select(ids);
    return change;
}

void Lasso::follow(const Change &change) {
    qsizetype replaced = 0;
    for (const QString &id : std::as_const(change.removed)) {
        replaced += m_selection.removeAll(id);
    }
    if (replaced == 0) {
        return;
    }
    for (const Stroke &stroke : change.added) {
        m_selection.append(stroke.id);
    }
    changed();
}

void Lasso::prune() {
    const qsizetype gone = m_selection.removeIf([this](const QString &id) { return m_sheet->find(id) == nullptr; });
    if (gone > 0) {
        changed();
    }
}

void Lasso::changed() const {
    if (m_told) {
        m_told();
    }
}

void marks(const QPolygonF &path, QRectF box, QColor color, double scale, double line, Mesh &mesh) {
    mesh.vertices.clear();
    mesh.indices.clear();
    if (!box.isEmpty()) {
        // Clear of the strokes' edges, so that its lines never hide them.
        const double clear = line * boxClearance;
        const QRectF shown = QRectF(box.topLeft() * scale, box.size() * scale).adjusted(-clear, -clear, clear, clear);
        quad(mesh, {shown.topLeft(), shown.topRight(), shown.bottomRight(), shown.bottomLeft()}, color, boxFill);
        piece(mesh, shown.topLeft(), shown.topRight(), line, color);
        piece(mesh, shown.topRight(), shown.bottomRight(), line, color);
        piece(mesh, shown.bottomRight(), shown.bottomLeft(), line, color);
        piece(mesh, shown.bottomLeft(), shown.topLeft(), line, color);
    }
    const qsizetype stride = std::max<qsizetype>(1, (path.size() + mostPieces - 1) / mostPieces);
    for (qsizetype i = stride; i < path.size(); i += stride) {
        piece(mesh, path.at(i - stride) * scale, path.at(i) * scale, line, color);
    }
}

} // namespace Ink
