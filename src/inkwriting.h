#pragma once

#include <QColor>
#include <QPointF>
#include <QtTypes>

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

#include "inklasso.h"
#include "inksheet.h"
#include "inkstroke.h"

namespace Ink {

// A pointer on the surface: its device's id and, for a touch, its point's
// (a pen's or a mouse's is its own negative number).
using Pointer = std::pair<qint64, int>;

// One event of a pointer, its place in ink units and its moment the event's own.
struct Input {
    QPointF at;
    float pressure = 0.5F;
    float xTilt = 0.0F;
    float yTilt = 0.0F;
    quint64 ms = 0;
};

// What a pointer that goes down writes with.
struct Nib {
    QColor color{Qt::black};
    double width = 2.0;
    bool pressured = false;
    bool tilted = false;
};

// What it erases with: a circle, in ink units.
struct Rubber {
    double radius = 12.0;
    Erasing how = Erasing::Parts;
};

// The pointers that are down on a sheet (#78), and what each does: writes a
// stroke, which goes on the sheet when the pointer lifts, erases along its
// way, one step of undo for the sweep, or draws the lasso's path or drags
// its selection (#129). Each answers the change it made to the sheet, none
// while a stroke is still being written.
class Writing {
public:
    explicit Writing(Sheet &sheet, Lasso *lasso = nullptr) : m_sheet(&sheet), m_lasso(lasso) {}

    void write(Pointer pointer, const Input &input, const Nib &nib, bool touch);
    Change erase(Pointer pointer, const Input &input, Rubber rubber, bool touch);
    // The lasso, `grab` its reach beyond the selection: one pointer at a
    // time, false (and nothing done) while another has it or there is none.
    bool lasso(Pointer pointer, const Input &input, double grab, bool touch);
    Change move(Pointer pointer, const Input &input);
    // The last event of a pointer: its stroke goes on the sheet, its sweep ends.
    Change lift(Pointer pointer, const Input &input);
    // A pointer that was no stroke after all (a palm): what it wrote goes.
    void drop(Pointer pointer);
    // Drops every touch, answering which.
    std::vector<Pointer> dropTouches();

    [[nodiscard]] bool has(Pointer pointer) const { return m_live.contains(pointer); }
    [[nodiscard]] bool empty() const { return m_live.empty(); }
    // Whether a pen or a mouse is down, not a finger.
    [[nodiscard]] bool toolDown() const;
    // The strokes being written, in the order they began.
    [[nodiscard]] std::vector<const Stroke *> strokes() const;

private:
    struct Live {
        Stroke stroke;
        bool erasing = false;
        bool touch = false;
        quint64 startMs = 0;
        QPointF last;
        Rubber rubber;
        bool lassoing = false;
    };

    Change extend(Live &live, const Input &input);
    Change sweep(Live &live, QPointF to);

    Sheet *m_sheet; // never null: the sheet the surface has
    Lasso *m_lasso; // the sheet's lasso, if it has one
    std::map<Pointer, Live> m_live;
};

} // namespace Ink
