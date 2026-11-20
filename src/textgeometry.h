#pragma once

#include <QList>
#include <QRectF>
#include <QStringView>
#include <utility>

class QTextDocument;

// Where characters of a laid-out text stand (#81): the rectangles a stretch
// of it covers, a rectangle a line, and the lines it is set in, each with its
// box and the characters on it, for a pen that writes over them. All in the
// document's own coordinates, which are those of the TextEdit showing it.
namespace TextGeometry {

struct Line {
    QRectF box; // from its first character to its last, as tall as the line
    int start = 0;
    int end = 0; // the character past its last
};

// The rectangles from character `start` to `end`, one a line it is on.
[[nodiscard]] QList<QRectF> rects(const QTextDocument &document, int start, int end);
// The lines that lie within `area`, or partly in it, top to bottom.
[[nodiscard]] QList<Line> linesIn(const QTextDocument &document, const QRectF &area);
// The character nearest `x` on a line.
[[nodiscard]] int positionOn(const QTextDocument &document, const Line &line, double x);
// A stretch of a text widened to whole words at both ends, without the
// spaces around it: {start, end}.
[[nodiscard]] std::pair<int, int> wholeWords(QStringView text, int start, int end);

} // namespace TextGeometry
