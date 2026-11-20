#include "pagetext.h"

#include <algorithm>
#include <limits>

namespace {

[[nodiscard]] QRectF shownBox(const PageWord &word, QSizeF shown) {
    return {word.box.x() * shown.width(), word.box.y() * shown.height(), word.box.width() * shown.width(),
            word.box.height() * shown.height()};
}

// Whether a word goes on along a line: beside it, sharing most of its height.
[[nodiscard]] bool along(const QRectF &line, const QRectF &word) {
    const double shared = std::min(line.bottom(), word.bottom()) - std::max(line.top(), word.top());
    return shared >= 0.5 * std::min(line.height(), word.height()) && word.left() >= line.right() - line.height();
}

[[nodiscard]] bool onLine(const PageWord &word, const TextGeometry::Line &line) { return word.start >= line.start && word.end <= line.end; }

} // namespace

QList<TextGeometry::Line> PageText::lines(const QList<PageWord> &words, QSizeF shown) {
    QList<TextGeometry::Line> found;
    for (const PageWord &word : words) {
        const QRectF box = shownBox(word, shown);
        if (!found.isEmpty() && along(found.last().box, box)) {
            found.last().box |= box;
            found.last().end = static_cast<int>(word.end);
            continue;
        }
        found << TextGeometry::Line{.box = box, .start = static_cast<int>(word.start), .end = static_cast<int>(word.end)};
    }
    return found;
}

QList<QRectF> PageText::rects(const QList<PageWord> &words, int start, int end, QSizeF shown) {
    QList<QRectF> found;
    for (const TextGeometry::Line &line : lines(words, shown)) {
        QRectF covered;
        for (const PageWord &word : words)
            if (onLine(word, line) && word.end > start && word.start < end)
                covered |= shownBox(word, shown);
        if (!covered.isEmpty())
            found << covered;
    }
    return found;
}

int PageText::positionOn(const QList<PageWord> &words, const TextGeometry::Line &line, double x, bool end, QSizeF shown) {
    const PageWord *nearest = nullptr;
    double best = std::numeric_limits<double>::max();
    for (const PageWord &word : words) {
        if (!onLine(word, line))
            continue;
        const QRectF box = shownBox(word, shown);
        // How far the point is beside the word, 0 on it.
        const double off = std::max({box.left() - x, x - box.right(), 0.0});
        if (off < best) {
            best = off;
            nearest = &word;
        }
    }
    if (!nearest)
        return end ? line.end : line.start;
    return static_cast<int>(end ? nearest->end : nearest->start);
}
