#pragma once

#include <QByteArray>
#include <QImage>

#include <span>

#include "inkstroke.h"

// Ink as a picture (#84), for a vision model to read: the strokes in black on
// white, whatever their colour, fitted with a margin into a picture of at most
// `longest` pixels a side (never enlarged past twice their own size), each as
// wide as it was pressed. Nothing of where the ink is kept or shown is needed.
namespace Ink {

inline constexpr int pictureSide = 1024;

// Empty (a null image) when there are no points to draw.
[[nodiscard]] QImage picture(std::span<const Stroke> strokes, int longest = pictureSide);
// The same as a PNG file's bytes; empty when there is nothing to draw.
[[nodiscard]] QByteArray png(std::span<const Stroke> strokes, int longest = pictureSide);

} // namespace Ink
