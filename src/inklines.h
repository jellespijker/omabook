#pragma once

#include <QPointF>
#include <QRectF>

#include <optional>
#include <span>

#include "inkstroke.h"

class QTextDocument;

// Free ink beside the lines of a flowing text (#152, ADR 0010): where a stroke
// drawn on a laid-out chapter is anchored, and how the strokes of an anchor
// stand on the chapter as it is laid out now, at any size of type, width or
// layout. All in the document's own coordinates, which are those of the
// TextEdit showing it (TextGeometry's).
//
// A stroke is kept in ink units of its line's height (`lineUnits` a line), so
// it grows and shrinks with the type and stays as readable beside it. One whose
// middle is beyond an edge of the column is in that margin: it is measured from
// that edge, outward, and its line's top, so it stays in the margin however
// the lines are broken. One among the lines is measured from the first of the
// words under or over it, so it goes where those words go.
namespace InkLines {

inline constexpr double lineUnits = 100.0;

enum class Where { Margin, Lines };

// The column the text is set in, in the document's coordinates: its edges,
// and how far the page reaches past each, which a margin's ink is kept within.
struct Column {
    double left = 0;
    double right = 0;
    double leftRoom = 0;
    double rightRoom = 0;
};

// What strokes are anchored to: a margin beside a line, its characters the
// whole line; or the lines, at the words of one of them that a stroke is under
// or over. Characters of the document, the first and the one past the last.
struct Anchor {
    Where where = Where::Lines;
    int start = 0;
    int end = 0;
    bool operator==(const Anchor &) const = default;
};

// What a stroke drawn at `box` is anchored to. Its line is the one its middle
// is on or, between two, the nearer (the one above when they are as near).
// None for a document with no lines.
[[nodiscard]] std::optional<Anchor> anchorOf(const QTextDocument &document, const Column &column, const QRectF &box);

// How an anchor's strokes stand on the chapter as it is laid out now. In a
// margin narrower than its ink (large type, a narrow window), the ink of each
// side is drawn smaller, as much as it takes to stay within the page, and at
// its size again once there is room.
class Placing {
public:
    // Of the strokes `kept` on it; none when the document lays out none of the anchor's characters.
    [[nodiscard]] static std::optional<Placing> of(const QTextDocument &document, const Column &column, const Anchor &anchor,
                                                   std::span<const Ink::Stroke> kept);
    // A stroke as it is kept, as it is shown; and back.
    [[nodiscard]] Ink::Stroke shown(Ink::Stroke kept) const;
    [[nodiscard]] Ink::Stroke kept(Ink::Stroke shown) const;
    // The top of the anchor's line and its height, in pixels.
    [[nodiscard]] double top() const noexcept { return m_origin.y(); }
    [[nodiscard]] double lineHeight() const noexcept { return m_scale * lineUnits; }

private:
    struct Frame {
        QPointF origin;
        double scale = 1; // pixels to an ink unit
    };
    // Which frame a stroke is in: a margin's, by the side its middle is on.
    [[nodiscard]] Frame frameOfKept(const Ink::Stroke &stroke) const;
    [[nodiscard]] Frame frameOfShown(const Ink::Stroke &stroke) const;

    Where m_where = Where::Lines;
    QPointF m_origin;
    double m_scale = 1;
    Column m_column;
    double m_leftScale = 1;
    double m_rightScale = 1;
};

} // namespace InkLines
