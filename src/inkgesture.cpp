#include "inkgesture.h"

#include <QLineF>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numbers>
#include <optional>
#include <vector>

namespace {

using Ink::GestureKind;
using Ink::LineSpan;
using Ink::TextLine;

// What a stroke is, as its shape: its bounds, the length of its path, how
// far its end is from its start, and how often it turned back across.
struct Shape {
    QRectF bounds;
    double length = 0;
    double gap = 0;
    int turns = 0;
};

[[nodiscard]] double lineHeight(std::span<const TextLine> lines) {
    std::vector<double> heights;
    heights.reserve(lines.size());
    for (const TextLine &line : lines)
        heights.push_back(line.box.height());
    std::ranges::nth_element(heights, heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2));
    return std::max(1.0, heights[heights.size() / 2]);
}

// How often the pen turned back sideways by more than `least`: a scribble's back and forth.
[[nodiscard]] int turnsOf(std::span<const QPointF> points, double least) {
    int turns = 0;
    int heading = 0; // 1 rightwards, -1 leftwards, 0 before it went either way far enough
    double low = points.front().x();
    double high = low;
    double extreme = low;
    for (const QPointF &point : points) {
        const double x = point.x();
        if (heading == 0) {
            low = std::min(low, x);
            high = std::max(high, x);
            if (high - low >= least) {
                heading = x >= high ? 1 : -1;
                extreme = x;
            }
        } else if ((x - extreme) * heading > 0) {
            extreme = x;
        } else if (std::abs(x - extreme) >= least) {
            ++turns;
            heading = -heading;
            extreme = x;
        }
    }
    return turns;
}

[[nodiscard]] Shape shapeOf(std::span<const QPointF> points, double height) {
    Shape shape;
    double left = points.front().x();
    double right = left;
    double top = points.front().y();
    double bottom = top;
    for (size_t i = 0; i < points.size(); ++i) {
        left = std::min(left, points[i].x());
        right = std::max(right, points[i].x());
        top = std::min(top, points[i].y());
        bottom = std::max(bottom, points[i].y());
        if (i > 0)
            shape.length += QLineF(points[i - 1], points[i]).length();
    }
    shape.bounds = QRectF(QPointF(left, top), QPointF(right, bottom));
    shape.gap = QLineF(points.front(), points.back()).length();
    shape.turns = turnsOf(points, height * 0.3);
    return shape;
}

// The lines whose middle lies between two heights.
[[nodiscard]] QList<int> linesBetween(std::span<const TextLine> lines, double top, double bottom) {
    QList<int> found;
    for (size_t i = 0; i < lines.size(); ++i)
        if (const double middle = lines[i].box.center().y(); middle >= top && middle <= bottom)
            found << static_cast<int>(i);
    return found;
}

// A stretch of a line, kept within it; none when little of it is left.
[[nodiscard]] std::optional<LineSpan> within(std::span<const TextLine> lines, int line, double from, double to, double least) {
    const QRectF &box = lines[static_cast<size_t>(line)].box;
    const double start = std::max(from, box.left());
    const double end = std::min(to, box.right());
    if (end - start < least)
        return std::nullopt;
    return LineSpan{.line = line, .from = start, .to = end};
}

// Where the closed loop of the points crosses a height: its leftmost and rightmost.
[[nodiscard]] std::optional<std::pair<double, double>> crossings(std::span<const QPointF> points, double y) {
    std::optional<std::pair<double, double>> found;
    for (size_t i = 0; i < points.size(); ++i) {
        const QPointF &a = points[i];
        const QPointF &b = points[(i + 1) % points.size()];
        if ((a.y() > y) == (b.y() > y))
            continue;
        const double x = a.x() + ((y - a.y()) / (b.y() - a.y()) * (b.x() - a.x()));
        found = found ? std::pair{std::min(found->first, x), std::max(found->second, x)} : std::pair{x, x};
    }
    return found;
}

[[nodiscard]] Ink::Gesture spansOf(GestureKind kind, std::span<const TextLine> lines, const QList<int> &chosen,
                                   const std::function<std::pair<double, double>(int)> &across, double least) {
    Ink::Gesture gesture{.kind = kind, .spans = {}};
    for (const int line : chosen) {
        const auto [from, to] = across(line);
        if (const std::optional<LineSpan> span = within(lines, line, from, to, least))
            gesture.spans << *span;
    }
    if (gesture.spans.isEmpty())
        gesture.kind = GestureKind::None;
    return gesture;
}

[[nodiscard]] bool looped(const Shape &shape, double height) {
    const QRectF &box = shape.bounds;
    const double around = std::numbers::pi * (box.width() + box.height()) / 2;
    return shape.gap <= 0.35 * std::max(box.width(), box.height()) && shape.length >= 0.75 * around && box.height() >= 0.5 * height
           && box.width() >= height;
}

[[nodiscard]] bool barred(const Shape &shape, std::span<const TextLine> lines, double height) {
    const QRectF &box = shape.bounds;
    double left = lines.front().box.left();
    double right = lines.front().box.right();
    for (const TextLine &line : lines) {
        left = std::min(left, line.box.left());
        right = std::max(right, line.box.right());
    }
    const double x = box.center().x();
    return box.height() >= 1.2 * height && box.width() <= std::max(0.6 * height, 0.35 * box.height())
           && (x <= left + height || x >= right - height);
}

// The line a stroke along it is under or through: the one whose foot is nearest below its middle.
[[nodiscard]] std::optional<int> lineUnder(std::span<const TextLine> lines, double y) {
    std::optional<int> best;
    double nearest = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const QRectF &box = lines[i].box;
        if (y < box.top() + (0.35 * box.height()) || y > box.bottom() + (0.6 * box.height()))
            continue;
        if (const double off = std::abs(box.bottom() - y); !best || off < nearest) {
            best = static_cast<int>(i);
            nearest = off;
        }
    }
    return best;
}

} // namespace

Ink::Gesture Ink::gestureOf(std::span<const QPointF> points, std::span<const TextLine> lines) {
    if (points.size() < 2 || lines.empty())
        return {};
    const double height = lineHeight(lines);
    const Shape shape = shapeOf(points, height);
    const QRectF &box = shape.bounds;
    const double least = height * 0.5;
    const auto sideways = [&box](int) { return std::pair{box.left(), box.right()}; };
    if (shape.turns >= 4 && shape.length >= 2.5 * box.width()) {
        const double overlap = height * 0.3;
        return spansOf(GestureKind::Scribble, lines, linesBetween(lines, box.top() - overlap, box.bottom() + overlap), sideways, 1);
    }
    if (looped(shape, height)) {
        const auto inside = [&](int line) {
            return crossings(points, lines[static_cast<size_t>(line)].box.center().y()).value_or(std::pair{box.left(), box.right()});
        };
        return spansOf(GestureKind::Circle, lines, linesBetween(lines, box.top(), box.bottom()), inside, least);
    }
    if (barred(shape, lines, height)) {
        const auto whole = [&lines](int line) {
            const QRectF &of = lines[static_cast<size_t>(line)].box;
            return std::pair{of.left(), of.right()};
        };
        return spansOf(GestureKind::Bracket, lines, linesBetween(lines, box.top(), box.bottom()), whole, 1);
    }
    if (box.width() >= 1.5 * height && box.height() <= 0.7 * height && shape.length <= 1.4 * box.width()) {
        if (const std::optional<int> line = lineUnder(lines, box.center().y()))
            return spansOf(GestureKind::Underline, lines, {*line}, sideways, least);
    }
    return {};
}
