#include "highlightcolors.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double textContrast = 4.5;
constexpr double markContrast = 3.0;

[[nodiscard]] double channel(double value) { return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); }

[[nodiscard]] double luminance(const QColor &color) {
    return (0.2126 * channel(color.redF())) + (0.7152 * channel(color.greenF())) + (0.0722 * channel(color.blueF()));
}

[[nodiscard]] QColor over(const QColor &top, const QColor &bottom, double alpha) {
    const auto mix = [alpha](double a, double b) { return (a * alpha) + (b * (1 - alpha)); };
    return QColor::fromRgbF(static_cast<float>(mix(top.redF(), bottom.redF())), static_cast<float>(mix(top.greenF(), bottom.greenF())),
                            static_cast<float>(mix(top.blueF(), bottom.blueF())));
}

} // namespace

const QList<HighlightColors::Color> &HighlightColors::all() {
    static const QList<Color> colors{
        {.key = QStringLiteral("yellow"), .color = QColor(0xF0, 0xE4, 0x42), .dashes = {}},
        {.key = QStringLiteral("blue"), .color = QColor(0x56, 0xB4, 0xE9), .dashes = {4, 2}},
        {.key = QStringLiteral("green"), .color = QColor(0x00, 0x9E, 0x73), .dashes = {1, 2}},
        {.key = QStringLiteral("orange"), .color = QColor(0xE6, 0x9F, 0x00), .dashes = {5, 2, 1, 2}},
        {.key = QStringLiteral("purple"), .color = QColor(0xCC, 0x79, 0xA7), .dashes = {9, 3}},
    };
    return colors;
}

bool HighlightColors::known(QStringView key) {
    return key.isEmpty() || std::ranges::any_of(all(), [key](const Color &color) { return color.key == key; });
}

const HighlightColors::Color &HighlightColors::of(QStringView key) {
    const auto found = std::ranges::find_if(all(), [key](const Color &color) { return color.key == key; });
    return found == all().cend() ? all().first() : *found;
}

QString HighlightColors::stored(QStringView key) { return !known(key) || key == all().first().key ? QString() : key.toString(); }

double HighlightColors::contrast(const QColor &one, const QColor &other) {
    const double a = luminance(one);
    const double b = luminance(other);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

QColor HighlightColors::tint(QStringView key, const QColor &background, const QColor &text) {
    const QColor &color = of(key).color;
    // From strong to faint: the strongest that keeps the text readable.
    // On a page whose own contrast is low (a dim theme) it is very thin, and
    // the line under the words says which colour it is.
    for (int percent = 60; percent > 4; percent -= 2)
        if (const QColor laid = over(color, background, percent / 100.0); contrast(laid, text) >= textContrast)
            return laid;
    return over(color, background, 0.04);
}

QColor HighlightColors::line(QStringView key, const QColor &background) {
    QColor color = of(key).color;
    // Darker on a light page, lighter on a dark one, a step at a time.
    const bool lightPage = luminance(background) > 0.4;
    for (int step = 0; step < 20 && contrast(color, background) < markContrast; ++step)
        color = lightPage ? color.darker(112) : color.lighter(112);
    return color;
}
