#include "chronologyframe.h"

#include <array>

using Chronology::Chart::Mark;

QColor ChronologyFrame::faded(QColor color, double alpha) {
    color.setAlphaF(static_cast<float>(color.alphaF() * alpha));
    return color;
}

void ChronologyFrame::Shapes::mark(const Mark &mark, double from, double to, double y, const Look &look) {
    const double half = mark.span ? barHeight / 2 : markSize;
    const double left = mark.span ? from : from - markSize;
    const double wide = mark.span ? to - from : 2 * markSize;
    const auto box = [&](double out, const QColor &color) { rect(left - out, y - half - out, wide + 2 * out, 2 * half + 2 * out, color); };
    if (look.grow > 0)
        box(look.grow, look.ring);
    if (mark.contradicted)
        box(2, look.trouble);
    box(0, look.fill);
    if (look.hollow.isValid())
        box(-2, look.hollow);
    if (mark.openFrom)
        runsOn(from, y, -1, look.fill);
    if (mark.openTo)
        runsOn(to, y, 1, look.fill);
}

void ChronologyFrame::Shapes::runsOn(double at, double y, int way, const QColor &color) {
    for (const auto [step, alpha] : std::array<std::pair<int, double>, 3>{{{0, 0.6}, {1, 0.35}, {2, 0.15}}})
        rect(way < 0 ? at - (step + 1) * 5.0 : at + step * 5.0, y - barHeight / 2, 5, barHeight, faded(color, alpha));
}

void ChronologyFrame::Shapes::split(double x, double from, double to, const QColor &color) {
    if (to > from)
        rect(x - 1, from, 2, to - from, color);
    rect(x - 4, to - 4, 8, 8, color);
}
