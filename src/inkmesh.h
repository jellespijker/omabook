#pragma once

#include <QtTypes>

#include <vector>

#include "inkstroke.h"

// A stroke as triangles to draw (#78): its points smoothed into a curve
// (quadratic pieces through the middles between them, as a pen's line looks,
// with no lag: the curve ends at the last point), as wide at each place as
// the pressure there makes it, with round ends and an edge one device pixel
// soft, so that it is smooth without the window's multisampling. Made apart
// from the scene graph so that it can be tested, and timed, on its own.
namespace Ink {

// Laid out as the scene graph's coloured point (QSGGeometry::ColoredPoint2D),
// its colour premultiplied by its alpha.
struct Vertex {
    float x = 0.0F;
    float y = 0.0F;
    quint8 r = 0;
    quint8 g = 0;
    quint8 b = 0;
    quint8 a = 0;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<quint16> indices;
};

// The stroke in the coordinates it is shown in: `scale` of them to an ink
// unit, `feather` the width of its soft edge in them.
void tessellate(const Stroke &stroke, double scale, double feather, Mesh &mesh);

} // namespace Ink
