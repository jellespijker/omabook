#pragma once

#include <QList>
#include <QRectF>
#include <QSizeF>

#include "document.h"
#include "textgeometry.h"

// The words of a fixed page (a PDF's, Document::words) as lines of text, the
// way TextGeometry gives those of a flowing chapter (#81): so a stretch of a
// page is highlighted, shown and written over by the pen as one of a chapter
// is. Sizes are those the page is shown at, in pixels.
namespace PageText {

// Its words gathered into lines, top to bottom: words side by side that
// share most of their height are one line.
[[nodiscard]] QList<TextGeometry::Line> lines(const QList<PageWord> &words, QSizeF shown);
// The rectangles of the words from character `start` to `end`, one a line.
[[nodiscard]] QList<QRectF> rects(const QList<PageWord> &words, int start, int end, QSizeF shown);
// The character a line's word nearest `x` starts at, or (`end`) ends at.
[[nodiscard]] int positionOn(const QList<PageWord> &words, const TextGeometry::Line &line, double x, bool end, QSizeF shown);

} // namespace PageText
