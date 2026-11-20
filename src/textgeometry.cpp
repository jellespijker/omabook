#include "textgeometry.h"

#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <algorithm>

namespace {

// Each line of the blocks that lie between two heights, with where its block is.
template <typename Each> void eachLine(const QTextDocument &document, double top, double bottom, Each each) {
    const QAbstractTextDocumentLayout *layout = document.documentLayout();
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QRectF bounds = layout->blockBoundingRect(block);
        if (bounds.bottom() < top)
            continue;
        if (bounds.top() > bottom)
            break;
        const QTextLayout *laid = block.layout();
        for (int index = 0; laid && index < laid->lineCount(); ++index)
            each(block, bounds.topLeft(), laid->lineAt(index));
    }
}

[[nodiscard]] bool wordy(QChar c) { return c.isLetterOrNumber() || c == QChar(0x00AD) || c == u'\'' || c == QChar(0x2019); }

} // namespace

QList<QRectF> TextGeometry::rects(const QTextDocument &document, int start, int end) {
    QList<QRectF> found;
    if (end <= start)
        return found;
    const QTextBlock first = document.findBlock(start);
    const QTextBlock last = document.findBlock(end);
    if (!first.isValid())
        return found;
    const QAbstractTextDocumentLayout *layout = document.documentLayout();
    const double top = layout->blockBoundingRect(first).top();
    const double bottom = last.isValid() ? layout->blockBoundingRect(last).bottom() : top + document.size().height();
    eachLine(document, top, bottom, [&](const QTextBlock &block, QPointF at, const QTextLine &line) {
        const int from = std::max(start, block.position() + line.textStart());
        const int to = std::min(end, block.position() + line.textStart() + line.textLength());
        if (to <= from)
            return;
        const double a = line.cursorToX(from - block.position());
        const double b = line.cursorToX(to - block.position());
        found << QRectF(at.x() + std::min(a, b), at.y() + line.y(), std::max(2.0, std::abs(b - a)), line.height());
    });
    return found;
}

QList<TextGeometry::Line> TextGeometry::linesIn(const QTextDocument &document, const QRectF &area) {
    QList<Line> found;
    eachLine(document, area.top(), area.bottom(), [&](const QTextBlock &block, QPointF at, const QTextLine &line) {
        const QRectF box(at.x() + line.x(), at.y() + line.y(), line.naturalTextWidth(), line.height());
        if (box.bottom() < area.top() || box.top() > area.bottom() || line.textLength() == 0)
            return;
        found << Line{
            .box = box, .start = block.position() + line.textStart(), .end = block.position() + line.textStart() + line.textLength()};
    });
    return found;
}

int TextGeometry::positionOn(const QTextDocument &document, const Line &line, double x) {
    const QTextBlock block = document.findBlock(line.start);
    if (!block.isValid() || !block.layout())
        return line.start;
    const QPointF at = document.documentLayout()->blockBoundingRect(block).topLeft();
    const QTextLine laid = block.layout()->lineForTextPosition(line.start - block.position());
    if (!laid.isValid())
        return line.start;
    return std::clamp(block.position() + laid.xToCursor(x - at.x()), line.start, line.end);
}

std::pair<int, int> TextGeometry::wholeWords(QStringView text, int start, int end) {
    const auto size = static_cast<int>(text.size());
    start = std::clamp(start, 0, size);
    end = std::clamp(end, start, size);
    while (start < end && text[start].isSpace())
        ++start;
    while (end > start && text[end - 1].isSpace())
        --end;
    while (start > 0 && wordy(text[start - 1]) && start < size && wordy(text[start]))
        --start;
    while (end < size && end > 0 && wordy(text[end - 1]) && wordy(text[end]))
        ++end;
    return {start, end};
}
