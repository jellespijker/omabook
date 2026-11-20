#pragma once

#include <QList>
#include <QPointF>
#include <QRectF>

#include <span>

// What a stroke of the pen means when it is made over lines of text (#81):
// a line under (or through) words highlights them, a loop around words
// highlights what it encloses, a bar in the margin beside lines highlights
// those lines, and a scribble back and forth over words takes away the
// highlights there. Anything else means nothing here, and is left to whoever
// else reads the pen (a margin note, #82). Only the shape and where it lies
// against the lines count: nothing here knows of a book, a page or a colour.
//
// Sizes are measured against the lines' height, so a gesture means the same
// at any size of type and any zoom.
namespace Ink {

// One line of text as it stands where the pen writes: its box, left to right
// and top to bottom, in the stroke's own coordinates.
struct TextLine {
    QRectF box;
};

enum class GestureKind { None, Underline, Circle, Bracket, Scribble };

// A stretch of one line, from one x to another.
struct LineSpan {
    int line = 0;
    double from = 0;
    double to = 0;
    bool operator==(const LineSpan &) const = default;
};

struct Gesture {
    GestureKind kind = GestureKind::None;
    // Top to bottom: what it marks (or, scribbled, unmarks) of each line.
    QList<LineSpan> spans;
};

// What the points of one stroke, in order, mean over these lines (top to bottom).
[[nodiscard]] Gesture gestureOf(std::span<const QPointF> points, std::span<const TextLine> lines);

} // namespace Ink
