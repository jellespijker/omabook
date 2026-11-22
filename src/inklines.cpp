#include "inklines.h"

#include <QTextDocument>

#include <algorithm>
#include <cmath>
#include <utility>

#include "textgeometry.h"

namespace {

using InkLines::Column;

// How far above and below a stroke its line is looked for at first, in pixels.
constexpr double reach = 400;

[[nodiscard]] double distanceTo(const QRectF &box, double y) {
    if (y >= box.top() && y <= box.bottom())
        return 0;
    return std::min(std::abs(y - box.top()), std::abs(y - box.bottom()));
}

// The line nearest a height: the first of those as near, so the one above.
// What lies just under a line, its top part in the next one's box or in the
// space between them (an underline), is beside it: a line reaches down
// `under` of its height further, and up as much less.
[[nodiscard]] std::optional<TextGeometry::Line> lineNear(const QTextDocument &document, const Column &column, double y) {
    constexpr double under = 0.3;
    QList<TextGeometry::Line> lines = TextGeometry::linesIn(document, QRectF(column.left, y - reach, 1, 2 * reach));
    if (lines.isEmpty())
        lines = TextGeometry::linesIn(document, QRectF(QPointF(column.left, 0), document.size()));
    const auto nearest = std::ranges::min_element(
        lines, {}, [y](const TextGeometry::Line &line) { return distanceTo(line.box.translated(0, line.box.height() * under), y); });
    return nearest != lines.cend() ? std::optional(*nearest) : std::nullopt;
}

// The words of a line from one character to another, at least one: the word
// nearest them when there is none between them, the whole line when it has none.
[[nodiscard]] std::pair<int, int> wordsOn(const QString &text, const TextGeometry::Line &line, int from, int to) {
    const auto [start, end] = TextGeometry::wholeWords(text, std::min(from, to), std::max(from, to));
    if (end > start)
        return {start, end};
    for (int away = 0; away <= line.end - line.start; ++away)
        for (const int at : {from + away, from - away - 1})
            if (at >= line.start && at < line.end && std::cmp_less(at, text.size()) && text[at].isLetterOrNumber())
                return TextGeometry::wholeWords(text, at, at + 1);
    return {line.start, line.end};
}

// How far a margin's strokes reach from its edge, in ink units: those of the left one, and of the right.
[[nodiscard]] std::pair<double, double> reachOf(std::span<const Ink::Stroke> kept) {
    double left = 0;
    double right = 0;
    for (const Ink::Stroke &stroke : kept) {
        const QRectF box = Ink::bounds(stroke);
        if (box.center().x() < 0)
            left = std::max(left, -box.left());
        else
            right = std::max(right, box.right());
    }
    return {left, right};
}

[[nodiscard]] double fitted(double scale, double room, double reached) {
    return reached > 0 && reached * scale > room ? std::max(0.0, room) / reached : scale;
}

[[nodiscard]] Ink::Stroke moved(Ink::Stroke stroke, QPointF from, double factor, QPointF to) {
    stroke.width *= factor;
    for (Ink::Point &point : stroke.points)
        point.at = ((point.at - from) * factor) + to;
    return stroke;
}

} // namespace

std::optional<InkLines::Anchor> InkLines::anchorOf(const QTextDocument &document, const Column &column, const QRectF &box) {
    const QPointF middle = box.center();
    const std::optional<TextGeometry::Line> line = lineNear(document, column, middle.y());
    if (!line)
        return std::nullopt;
    if (middle.x() < column.left || middle.x() > column.right)
        return Anchor{.where = Where::Margin, .start = line->start, .end = line->end};
    const int from = TextGeometry::positionOn(document, *line, box.left());
    const int to = TextGeometry::positionOn(document, *line, box.right());
    const auto [start, end] = wordsOn(document.toPlainText(), *line, from, to);
    return Anchor{.where = Where::Lines, .start = start, .end = end};
}

std::optional<InkLines::Placing> InkLines::Placing::of(const QTextDocument &document, const Column &column, const Anchor &anchor,
                                                       std::span<const Ink::Stroke> kept) {
    const QList<QRectF> rects = TextGeometry::rects(document, anchor.start, std::max(anchor.end, anchor.start + 1));
    if (rects.isEmpty() || rects.first().height() <= 0)
        return std::nullopt;
    const QRectF first = rects.first();
    Placing placing;
    placing.m_where = anchor.where;
    placing.m_column = column;
    placing.m_scale = first.height() / lineUnits;
    placing.m_origin = anchor.where == Where::Lines ? first.topLeft() : QPointF(column.left, first.top());
    if (anchor.where == Where::Margin) {
        const auto [left, right] = reachOf(kept);
        placing.m_leftScale = fitted(placing.m_scale, column.leftRoom, left);
        placing.m_rightScale = fitted(placing.m_scale, column.rightRoom, right);
    }
    return placing;
}

InkLines::Placing::Frame InkLines::Placing::frameOfKept(const Ink::Stroke &stroke) const {
    if (m_where == Where::Lines)
        return {.origin = m_origin, .scale = m_scale};
    if (Ink::bounds(stroke).center().x() < 0)
        return {.origin = QPointF(m_column.left, m_origin.y()), .scale = m_leftScale};
    return {.origin = QPointF(m_column.right, m_origin.y()), .scale = m_rightScale};
}

InkLines::Placing::Frame InkLines::Placing::frameOfShown(const Ink::Stroke &stroke) const {
    if (m_where == Where::Lines)
        return {.origin = m_origin, .scale = m_scale};
    if (Ink::bounds(stroke).center().x() < (m_column.left + m_column.right) / 2)
        return {.origin = QPointF(m_column.left, m_origin.y()), .scale = m_leftScale};
    return {.origin = QPointF(m_column.right, m_origin.y()), .scale = m_rightScale};
}

Ink::Stroke InkLines::Placing::shown(Ink::Stroke kept) const {
    const Frame frame = frameOfKept(kept);
    return moved(std::move(kept), {}, frame.scale, frame.origin);
}

Ink::Stroke InkLines::Placing::kept(Ink::Stroke shown) const {
    const Frame frame = frameOfShown(shown);
    // A margin with no room at all draws its ink at nothing: what is drawn there is kept at the line's own size.
    const double scale = frame.scale > 0 ? frame.scale : m_scale;
    return moved(std::move(shown), frame.origin, 1 / scale, {});
}
