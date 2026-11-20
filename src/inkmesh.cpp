#include "inkmesh.h"

#include <QLineF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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
    void vertex(QPointF at, double alpha) {
        const double a = alpha * m_color.alphaF();
        const auto channel = [a](float value) { return static_cast<quint8>(std::lround(value * a * 255.0)); };
        m_mesh->vertices.push_back(Vertex{.x = static_cast<float>(at.x()),
                                          .y = static_cast<float>(at.y()),
                                          .r = channel(m_color.redF()),
                                          .g = channel(m_color.greenF()),
                                          .b = channel(m_color.blueF()),
                                          .a = static_cast<quint8>(std::lround(a * 255.0))});
    }
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

} // namespace

void tessellate(const Stroke &stroke, double scale, double feather, Mesh &mesh) {
    Builder out(mesh, stroke.color, feather);
    const std::vector<Sample> samples = curve(centres(stroke, scale));
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
