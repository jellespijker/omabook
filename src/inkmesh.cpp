#include "inkmesh.h"

#include <QLineF>
#include <QPainterPath>
#include <QPolygonF>
// The triangles of a shape however it crosses itself: Qt's own, as its
// OpenGL painting fills a path with; no public header has it.
#include <private/qtriangulator_p.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <span>
#include <utility>

namespace Ink {

namespace {

// A place on the curve: where, and half the line's width there.
struct Sample {
    QPointF at;
    double half = 0.0;
};

// Points nearer than this to the one before add nothing a reader sees.
constexpr double closest = 0.5;
// Most vertices a stroke may have with 16-bit indices, the scene graph's cheapest.
constexpr std::size_t mostSamples = 15000;

QPointF unit(QPointF v) {
    const double length = std::hypot(v.x(), v.y());
    return length > 0.0 ? v / length : QPointF(1.0, 0.0);
}

std::vector<Sample> centres(const Stroke &stroke, double scale) {
    std::vector<Sample> kept;
    kept.reserve(stroke.points.size());
    for (const Point &point : stroke.points) {
        const Sample sample{point.at * scale, widthAt(stroke, point) * scale / 2.0};
        if (!kept.empty() && QLineF(kept.back().at, sample.at).length() < closest) {
            // The last point always counts: the line ends where the pen lifted.
            if (&point == &stroke.points.back() && kept.size() > 1) {
                kept.back() = sample;
            }
            continue;
        }
        kept.push_back(sample);
    }
    return kept;
}

// Quadratic pieces through the middles between the points.
std::vector<Sample> curve(const std::vector<Sample> &points) {
    if (points.size() < 3) {
        return points;
    }
    const std::size_t n = points.size();
    const auto most = static_cast<double>(std::clamp<std::size_t>(mostSamples / n, 1, 8));
    std::vector<Sample> out{points.front()};
    const auto middle = [](const Sample &a, const Sample &b) { return Sample{(a.at + b.at) / 2.0, (a.half + b.half) / 2.0}; };
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const Sample from = i == 1 ? points.front() : middle(points.at(i - 1), points.at(i));
        const Sample &control = points.at(i);
        const Sample to = i + 2 == n ? points.back() : middle(points.at(i), points.at(i + 1));
        const double length = QLineF(from.at, control.at).length() + QLineF(control.at, to.at).length();
        const auto steps = static_cast<int>(std::clamp(std::ceil(length / 3.0), 1.0, most));
        for (int s = 1; s <= steps; ++s) {
            const double t = static_cast<double>(s) / steps;
            const double a = (1.0 - t) * (1.0 - t);
            const double b = 2.0 * t * (1.0 - t);
            const double c = t * t;
            out.push_back(Sample{from.at * a + control.at * b + to.at * c, from.half * a + control.half * b + to.half * c});
        }
    }
    return out;
}

// A vertex of `color`, `alpha` of it, premultiplied as the scene graph wants.
Vertex colored(QPointF at, QColor color, double alpha) {
    const double a = alpha * color.alphaF();
    const auto channel = [a](float value) { return static_cast<quint8>(std::lround(value * a * 255.0)); };
    return Vertex{.x = static_cast<float>(at.x()),
                  .y = static_cast<float>(at.y()),
                  .r = channel(color.redF()),
                  .g = channel(color.greenF()),
                  .b = channel(color.blueF()),
                  .a = static_cast<quint8>(std::lround(a * 255.0))};
}

class Builder {
public:
    Builder(Mesh &mesh, QColor color, double feather) : m_mesh(&mesh), m_color(color), m_feather(feather) {
        m_mesh->vertices.clear();
        m_mesh->indices.clear();
    }

    // The four across the line at a sample: soft edge, line, line, soft edge.
    void across(const Sample &sample, QPointF normal) {
        const auto [inner, alpha] = innerOf(sample.half);
        const double outer = sample.half + m_feather / 2.0;
        vertex(sample.at + normal * outer, 0.0);
        vertex(sample.at + normal * inner, alpha);
        vertex(sample.at - normal * inner, alpha);
        vertex(sample.at - normal * outer, 0.0);
    }
    // Joins the last two sets of four.
    void band() {
        const auto base = static_cast<quint16>(m_mesh->vertices.size() - 8);
        for (quint16 j = 0; j < 3; ++j) {
            quad(base + j, base + j + 1, base + 4 + j, base + 4 + j + 1);
        }
    }
    // Half a round end (or a whole dot), from `normal` round through `away`.
    void end(const Sample &sample, QPointF normal, QPointF away, double turn) {
        const auto [inner, alpha] = innerOf(sample.half);
        const double outer = sample.half + m_feather / 2.0;
        const int steps = std::clamp(static_cast<int>(std::ceil(outer * turn / 2.0)), 4, 24);
        const auto centre = static_cast<quint16>(m_mesh->vertices.size());
        vertex(sample.at, alpha);
        for (int k = 0; k <= steps; ++k) {
            const double angle = turn * k / steps;
            const QPointF direction = normal * std::cos(angle) + away * std::sin(angle);
            vertex(sample.at + direction * inner, alpha);
            vertex(sample.at + direction * outer, 0.0);
            if (k > 0) {
                const auto in = static_cast<quint16>(centre + 2 * k + 1);
                const auto before = static_cast<quint16>(in - 2);
                triangle(centre, before, in);
                quad(before, in, static_cast<quint16>(before + 1), static_cast<quint16>(in + 1));
            }
        }
    }
    [[nodiscard]] bool full(std::size_t more) const { return m_mesh->vertices.size() + more > std::numeric_limits<quint16>::max(); }

private:
    struct Inner {
        double offset = 0.0;
        double alpha = 1.0;
    };
    // A line thinner than the soft edge is drawn fainter instead of thinner.
    [[nodiscard]] Inner innerOf(double half) const {
        const double soft = m_feather / 2.0;
        return half >= soft ? Inner{half - soft, 1.0} : Inner{0.0, soft > 0.0 ? half / soft : 1.0};
    }
    void vertex(QPointF at, double alpha) { m_mesh->vertices.push_back(colored(at, m_color, alpha)); }
    void triangle(quint16 a, quint16 b, quint16 c) { m_mesh->indices.insert(m_mesh->indices.end(), {a, b, c}); }
    // a-b along one side, c-d along the other.
    void quad(quint16 a, quint16 b, quint16 c, quint16 d) {
        triangle(a, c, b);
        triangle(b, c, d);
    }

    Mesh *m_mesh; // never null
    QColor m_color;
    double m_feather;
};

QPointF normalOf(QPointF tangent) { return {-tangent.y(), tangent.x()}; }

double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }

// Whether it turns the same way at every corner.
bool convex(const QPolygonF &polygon) {
    const qsizetype n = polygon.size();
    int left = 0;
    int right = 0;
    for (qsizetype i = 0; i < n; ++i) {
        const double turn = cross(polygon.at((i + 1) % n) - polygon.at(i), polygon.at((i + 2) % n) - polygon.at((i + 1) % n));
        left += turn > 0.0 ? 1 : 0;
        right += turn < 0.0 ? 1 : 0;
    }
    return left == n || right == n;
}

// A highlighter's line as one shape (#130): the parts the pen's line is made
// of (a band between each two samples, round ends), each turned the same way
// so that none cancels another, as one outline of all of them; its triangles
// then cover each place once, so the colour's alpha is laid on once.
class Shape {
public:
    Shape(Mesh &mesh, QColor color, double feather) : m_mesh(&mesh), m_color(color), m_feather(feather) {
        m_mesh->vertices.clear();
        m_mesh->indices.clear();
        m_path.setFillRule(Qt::WindingFill);
    }

    void build(const std::vector<Sample> &samples) {
        const std::size_t last = samples.size() - 1;
        std::vector<QPointF> normals;
        normals.reserve(samples.size());
        for (std::size_t k = 0; k <= last; ++k) {
            normals.push_back(normalOf(unit(samples.at(std::min(k + 1, last)).at - samples.at(k == 0 ? 0 : k - 1).at)));
        }
        disc(samples.front());
        for (std::size_t k = 1; k <= last; ++k) {
            const Sample &a = samples.at(k - 1);
            const Sample &b = samples.at(k);
            const QPointF la = a.at + normals.at(k - 1) * inner(a.half);
            const QPointF ra = a.at - normals.at(k - 1) * inner(a.half);
            const QPointF lb = b.at + normals.at(k) * inner(b.half);
            const QPointF rb = b.at - normals.at(k) * inner(b.half);
            // Where the line turns sharply the band is folded: as two triangles, each whole.
            if (convex(QPolygonF{la, lb, rb, ra})) {
                part(QPolygonF{la, lb, rb, ra});
            } else {
                part(QPolygonF{la, lb, rb});
                part(QPolygonF{la, rb, ra});
            }
        }
        if (last > 0) {
            disc(samples.back());
        }
        fill();
    }

private:
    // The solid part reaches to where the pen's line begins to soften.
    [[nodiscard]] double inner(double half) const { return std::max(half - m_feather / 2.0, half / 2.0); }
    void part(QPolygonF polygon) {
        double area = 0.0;
        for (qsizetype i = 0; i < polygon.size(); ++i) {
            area += cross(polygon.at(i), polygon.at((i + 1) % polygon.size()));
        }
        if (area < 0.0) {
            std::ranges::reverse(polygon);
        }
        m_path.addPolygon(polygon);
        m_path.closeSubpath();
    }
    void disc(const Sample &sample) {
        const double radius = inner(sample.half);
        const int steps = std::clamp(static_cast<int>(std::ceil(radius * std::numbers::pi)), 8, 48);
        QPolygonF polygon;
        for (int k = 0; k < steps; ++k) {
            const double angle = 2.0 * std::numbers::pi * k / steps;
            polygon.append(sample.at + QPointF(std::cos(angle), std::sin(angle)) * radius);
        }
        part(polygon);
    }
    void fill() {
        const QPainterPath outline = m_path.simplified();
        const QTriangleSet triangles = qTriangulate(outline);
        const qsizetype count = triangles.vertices.size() / 2;
        if (count == 0 || std::cmp_greater(count, std::numeric_limits<quint16>::max())) {
            return;
        }
        for (qsizetype i = 0; i < count; ++i) {
            m_mesh->vertices.push_back(colored(QPointF(triangles.vertices.at(2 * i), triangles.vertices.at(2 * i + 1)), m_color, 1.0));
        }
        const auto size = static_cast<std::size_t>(triangles.indices.size());
        if (triangles.indices.type() == QVertexIndexVector::UnsignedShort) {
            const std::span indices{static_cast<const quint16 *>(triangles.indices.data()), size};
            m_mesh->indices.assign(indices.begin(), indices.end());
        } else {
            const std::span indices{static_cast<const quint32 *>(triangles.indices.data()), size};
            std::ranges::transform(indices, std::back_inserter(m_mesh->indices), [](quint32 i) { return static_cast<quint16>(i); });
        }
        if (m_feather > 0.0) {
            for (const QPolygonF &contour : outline.toSubpathPolygons()) {
                soften(contour, outline);
            }
        }
    }
    // A soft edge outside the outline, a device pixel wide: from the colour
    // at the outline to nothing, along each place's own outward direction so
    // that two edges' strips don't lie over each other.
    void soften(const QPolygonF &given, const QPainterPath &outline) {
        QPolygonF contour;
        for (const QPointF &at : given) {
            if (contour.isEmpty() || QLineF(contour.back(), at).length() > 1e-3) {
                contour.append(at);
            }
        }
        if (contour.size() > 1 && QLineF(contour.front(), contour.back()).length() <= 1e-3) {
            contour.removeLast();
        }
        const qsizetype n = contour.size();
        if (n < 3 || m_mesh->vertices.size() + 2 * static_cast<std::size_t>(n) > std::numeric_limits<quint16>::max()) {
            return;
        }
        // Which side of the contour is outside the shape (an outline goes
        // round a hole the other way), tried beside its longest edge.
        qsizetype longest = 0;
        for (qsizetype i = 1; i < n; ++i) {
            if (QLineF(contour.at(i), contour.at((i + 1) % n)).length()
                > QLineF(contour.at(longest), contour.at((longest + 1) % n)).length()) {
                longest = i;
            }
        }
        const QPointF from = contour.at(longest);
        const QPointF to = contour.at((longest + 1) % n);
        const double side = outline.contains((from + to) / 2.0 + normalOf(unit(to - from)) * 0.01) ? -1.0 : 1.0;
        const auto base = static_cast<quint16>(m_mesh->vertices.size());
        for (qsizetype i = 0; i < n; ++i) {
            const QPointF before = normalOf(unit(contour.at(i) - contour.at((i + n - 1) % n))) * side;
            const QPointF after = normalOf(unit(contour.at((i + 1) % n) - contour.at(i))) * side;
            const QPointF out = unit(before + after);
            // Mitred, but not past twice the width at a sharp corner.
            const double reach = m_feather / std::max(0.5, QPointF::dotProduct(out, after));
            m_mesh->vertices.push_back(colored(contour.at(i), m_color, 1.0));
            m_mesh->vertices.push_back(colored(contour.at(i) + out * reach, m_color, 0.0));
        }
        for (qsizetype i = 0; i < n; ++i) {
            const auto at = static_cast<quint16>(base + 2 * i);
            const auto next = static_cast<quint16>(base + 2 * ((i + 1) % n));
            m_mesh->indices.insert(m_mesh->indices.end(), {at, next, static_cast<quint16>(at + 1), static_cast<quint16>(at + 1), next,
                                                           static_cast<quint16>(next + 1)});
        }
    }

    Mesh *m_mesh; // never null
    QColor m_color;
    double m_feather;
    QPainterPath m_path;
};

} // namespace

void tessellate(const Stroke &stroke, double scale, double feather, Mesh &mesh) {
    const std::vector<Sample> samples = curve(centres(stroke, scale));
    if (stroke.highlighter) {
        Shape shape(mesh, stroke.color, feather);
        if (!samples.empty()) {
            shape.build(samples);
        }
        return;
    }
    Builder out(mesh, stroke.color, feather);
    if (samples.empty()) {
        return;
    }
    if (samples.size() == 1) {
        out.end(samples.front(), QPointF(1.0, 0.0), QPointF(0.0, 1.0), 2.0 * std::numbers::pi);
        return;
    }
    const std::size_t last = samples.size() - 1;
    const QPointF startTangent = unit(samples.at(1).at - samples.front().at);
    const QPointF endTangent = unit(samples.at(last).at - samples.at(last - 1).at);
    out.end(samples.front(), normalOf(startTangent), -startTangent, std::numbers::pi);
    for (std::size_t k = 0; k <= last && !out.full(4); ++k) {
        const QPointF tangent = unit(samples.at(std::min(k + 1, last)).at - samples.at(k == 0 ? 0 : k - 1).at);
        out.across(samples.at(k), normalOf(tangent));
        if (k > 0) {
            out.band();
        }
    }
    if (!out.full(64)) {
        out.end(samples.back(), -normalOf(endTangent), endTangent, std::numbers::pi);
    }
}

} // namespace Ink
