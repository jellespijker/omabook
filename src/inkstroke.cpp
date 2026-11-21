#include "inkstroke.h"

#include <QByteArray>
#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Ink {

namespace {

// Where along A to B (0 to 1) a circle covers the segment, when it does.
struct Span {
    double from = 0.0;
    double to = 0.0;
};

std::optional<Span> covered(QPointF a, QPointF b, QPointF centre, double radius) {
    const QPointF d = b - a;
    const QPointF f = a - centre;
    const double qa = QPointF::dotProduct(d, d);
    const double qc = QPointF::dotProduct(f, f) - radius * radius;
    if (qa <= 0.0) {
        return qc <= 0.0 ? std::optional<Span>{Span{0.0, 1.0}} : std::nullopt;
    }
    const double qb = 2.0 * QPointF::dotProduct(f, d);
    const double discriminant = qb * qb - 4.0 * qa * qc;
    if (discriminant < 0.0) {
        return std::nullopt;
    }
    const double root = std::sqrt(discriminant);
    const double from = std::max(0.0, (-qb - root) / (2.0 * qa));
    const double to = std::min(1.0, (-qb + root) / (2.0 * qa));
    return from <= to ? std::optional<Span>{Span{from, to}} : std::nullopt;
}

Point between(const Point &a, const Point &b, double t) {
    const auto mix = [t](double x, double y) { return x + (y - x) * t; };
    return Point{.at = a.at + (b.at - a.at) * t,
                 .pressure = static_cast<float>(mix(a.pressure, b.pressure)),
                 .xTilt = static_cast<float>(mix(a.xTilt, b.xTilt)),
                 .yTilt = static_cast<float>(mix(a.yTilt, b.yTilt)),
                 .ms = static_cast<quint32>(std::lround(mix(a.ms, b.ms)))};
}

// The eraser reaches the line's edge, not only its middle.
double reach(const Stroke &stroke, const Point &a, const Point &b, double radius) {
    return radius + std::max(widthAt(stroke, a), widthAt(stroke, b)) / 2.0;
}

// The pieces of a stroke, as it is cut.
class Pieces {
public:
    explicit Pieces(const Stroke &stroke) : m_stroke(&stroke) {}

    void extend(const Point &point) { m_current.push_back(point); }
    void finish() {
        // A piece of one point is a crumb the eraser left: gone with the rest.
        if (m_current.size() > 1) {
            m_pieces.push_back(Stroke{.id = newId(),
                                      .color = m_stroke->color,
                                      .width = m_stroke->width,
                                      .pressured = m_stroke->pressured,
                                      .tilted = m_stroke->tilted,
                                      .highlighter = m_stroke->highlighter,
                                      .began = m_stroke->began,
                                      .points = std::move(m_current)});
        }
        m_current.clear();
    }
    [[nodiscard]] std::vector<Stroke> take() && {
        finish();
        return std::move(m_pieces);
    }

private:
    const Stroke *m_stroke; // never null
    std::vector<Point> m_current;
    std::vector<Stroke> m_pieces;
};

} // namespace

void Change::merge(Change later) {
    for (const QString &id : std::as_const(later.removed)) {
        const auto found = std::ranges::find(added, id, &Stroke::id);
        if (found != added.end()) {
            added.erase(found);
        } else if (!removed.contains(id)) {
            removed.append(id);
        }
    }
    for (Stroke &stroke : later.added) {
        // Put back as it was (an erasing undone): what the batch removed is
        // simply not removed.
        if (removed.removeOne(stroke.id)) {
            continue;
        }
        const auto found = std::ranges::find(added, stroke.id, &Stroke::id);
        if (found != added.end()) {
            *found = std::move(stroke);
        } else {
            added.push_back(std::move(stroke));
        }
    }
}

QString newId() {
    // 72 bits: no two strokes of anyone's notebooks meet, in twelve characters.
    std::array<quint32, 3> words{};
    QRandomGenerator::global()->generate(words.begin(), words.end());
    QByteArray bytes;
    for (const quint32 word : words) {
        bytes.append(static_cast<char>(word & 0xffU));
        bytes.append(static_cast<char>((word >> 8U) & 0xffU));
        bytes.append(static_cast<char>((word >> 16U) & 0xffU));
    }
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

double widthAt(const Stroke &stroke, const Point &point) {
    if (!stroke.pressured) {
        return stroke.width;
    }
    return stroke.width * (0.4 + 1.2 * std::clamp(static_cast<double>(point.pressure), 0.0, 1.0));
}

QRectF bounds(const Stroke &stroke) {
    if (stroke.points.empty()) {
        return {};
    }
    const auto [left, right] = std::ranges::minmax(stroke.points, {}, [](const Point &p) { return p.at.x(); });
    const auto [top, bottom] = std::ranges::minmax(stroke.points, {}, [](const Point &p) { return p.at.y(); });
    const double margin = stroke.width * 0.8;
    return QRectF(QPointF(left.at.x(), top.at.y()), QPointF(right.at.x(), bottom.at.y())).adjusted(-margin, -margin, margin, margin);
}

bool touches(const Stroke &stroke, QPointF centre, double radius) {
    const auto &points = stroke.points;
    if (points.size() == 1) {
        return covered(points.front().at, points.front().at, centre, reach(stroke, points.front(), points.front(), radius)).has_value();
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (covered(points.at(i - 1).at, points.at(i).at, centre, reach(stroke, points.at(i - 1), points.at(i), radius))) {
            return true;
        }
    }
    return false;
}

std::optional<std::vector<Stroke>> cut(const Stroke &stroke, QPointF centre, double radius) {
    if (!touches(stroke, centre, radius)) {
        return std::nullopt;
    }
    const auto &points = stroke.points;
    Pieces pieces(stroke);
    if (points.size() == 1) {
        return std::move(pieces).take();
    }
    const double first = reach(stroke, points.front(), points.front(), radius);
    if (QPointF::dotProduct(points.front().at - centre, points.front().at - centre) > first * first) {
        pieces.extend(points.front());
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Point &a = points.at(i - 1);
        const Point &b = points.at(i);
        const auto span = covered(a.at, b.at, centre, reach(stroke, a, b, radius));
        if (!span) {
            pieces.extend(b);
            continue;
        }
        if (span->from > 0.0) {
            pieces.extend(between(a, b, span->from));
        }
        pieces.finish();
        if (span->to < 1.0) {
            pieces.extend(between(a, b, span->to));
            pieces.extend(b);
        }
    }
    return std::move(pieces).take();
}

} // namespace Ink
