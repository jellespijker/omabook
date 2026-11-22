#pragma once

#include <QColor>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QTextLayout>
#include <utility>

#include "chronologychart.h"

// What one frame of the chronology's picture (ChronologyCanvas) is made of:
// the shapes, all rectangles, which every renderer of the scene graph draws
// (the software one too, which draws no geometry of one's own), and the
// words, each laid out once and kept. Both are put down as time runs, and
// turned round in a right-to-left language.
namespace ChronologyFrame {

// Half a moment's mark, and a span's bar, in pixels.
inline constexpr double markSize = 5;
inline constexpr double barHeight = 10;

[[nodiscard]] QColor faded(QColor color, double alpha);

struct Shapes {
    QList<std::pair<QRectF, QColor>> rects;
    bool mirrored = false;
    double width = 0;

    // A rectangle whose left edge is at `left` as time runs.
    void rect(double left, double top, double w, double h, const QColor &color) {
        if (w > 0 && h > 0 && color.alpha() > 0)
            rects.append({QRectF(mirrored ? width - left - w : left, top, w, h), color});
    }
    // How a mark looks: its colour, the ring of the selected one or the one
    // pointed at (`grow` wide, none when 0), the ring of one in a
    // contradiction, and the inside of one of someone else's graph.
    struct Look {
        QColor fill, ring, trouble, hollow;
        double grow = 0;
    };
    // A mark from `from` to `to`, at `y` down: a bar for a span, a square about `from` for a moment.
    void mark(const Chronology::Chart::Mark &mark, double from, double to, double y, const Look &look);
    // An end of a span that runs on: the bar fades out past it, `way` -1 before it and 1 after.
    void runsOn(double at, double y, int way, const QColor &color);
    // Where a branch splits off its parent (#171), at `x`: a line from the
    // parent's lane (`from` down) to the branch's (`to`), and a square
    // where it begins.
    void split(double x, double from, double to, const QColor &color);
};

struct Words {
    struct Placed {
        QPointF at;
        QTextLayout *layout = nullptr;
    };
    QList<Placed> ink, muted, lit;
    bool mirrored = false;
    double width = 0;

    // A box of words, its left edge at `left` as time runs.
    void add(QList<Placed> &to, double left, double top, QTextLayout &layout) const {
        const double wide = layout.lineAt(0).naturalTextWidth();
        to.append({{mirrored ? width - left - wide : left, top}, &layout});
    }
};

} // namespace ChronologyFrame
