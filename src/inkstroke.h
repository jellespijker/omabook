#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QtTypes>

#include <cstddef>
#include <optional>
#include <vector>

// Ink as data (#78): strokes of points, each with its pressure, tilt and
// moment, the same wherever they are kept. The ink surface draws them; the
// notebook plugin keeps them in its part of the change log, book ink (#83)
// with its page. Nothing here knows where a stroke is stored or shown: its
// points are in ink units, which the one showing it scales (a pixel of a
// notebook's page, a point of a PDF page).
namespace Ink {

// The most points one stroke holds: a longer line goes on as a new stroke, so
// that the largest stroke, encoded, stays well inside one write to the change
// log (64 KiB) with room for others in the same batch.
inline constexpr std::size_t maxPoints = 2048;

struct Point {
    QPointF at;
    // 0 to 1; what a pen without pressure, a finger or a mouse draws with is
    // `Stroke::pressured == false`, not a pressure of its own.
    float pressure = 0.5F;
    // Degrees from upright, as Qt gives them (-60 to 60), when the pen says.
    float xTilt = 0.0F;
    float yTilt = 0.0F;
    // Milliseconds since the stroke began.
    quint32 ms = 0;

    friend bool operator==(const Point &, const Point &) = default;
};

struct Stroke {
    // Its key wherever strokes are kept (an item of a set in the change log).
    QString id;
    QColor color{Qt::black};
    // In ink units: the width at a middling pressure, and everywhere when
    // the stroke has no pressure.
    double width = 2.0;
    bool pressured = false;
    bool tilted = false;
    // A highlighter's (#130): its colour see-through (its alpha), and laid
    // on once, as one shape, so that where the line crosses or folds over
    // itself it is no darker than anywhere else.
    bool highlighter = false;
    // Milliseconds since the epoch: when it began, which also orders the
    // strokes one over the other.
    qint64 began = 0;
    std::vector<Point> points;

    friend bool operator==(const Stroke &, const Stroke &) = default;
};

// What one edit of a sheet does, and what one ink batch says: strokes added
// (each whole) and strokes removed (by id). Erasing part of a stroke is its
// removal and its pieces' addition, in one change.
struct Change {
    std::vector<Stroke> added;
    QStringList removed;

    [[nodiscard]] bool empty() const { return added.empty() && removed.isEmpty(); }
    // `later` after this one, as one change: a stroke added and removed again
    // within it is neither.
    void merge(Change later);
};

// A new stroke id: random, short, the same alphabet as base64url.
[[nodiscard]] QString newId();

// How wide a stroke is at a point, in ink units: from 0.4 to 1.6 times its
// width as the pressure goes from none to full, its width without pressure.
[[nodiscard]] double widthAt(const Stroke &stroke, const Point &point);
[[nodiscard]] QRectF bounds(const Stroke &stroke);

// Whether an eraser of `radius` at `centre` touches the stroke's line.
[[nodiscard]] bool touches(const Stroke &stroke, QPointF centre, double radius);
// The stroke with what an eraser of `radius` at `centre` covers taken out:
// none when it doesn't touch it, else the pieces left (each with a new id,
// perhaps none), cut where the eraser's edge crosses the line.
[[nodiscard]] std::optional<std::vector<Stroke>> cut(const Stroke &stroke, QPointF centre, double radius);

} // namespace Ink
