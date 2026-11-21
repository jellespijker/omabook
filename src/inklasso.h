#pragma once

#include <QColor>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <vector>

#include "inkmesh.h"
#include "inksheet.h"
#include "inkstroke.h"

// The lasso of an ink surface (#129): a path drawn around strokes selects
// them, and the selection is moved, recoloured, taken away or copied, each
// one step of undo and one change of the sheet. A change keeps the shape of
// every other: the strokes it changes are removed and come back as new ones
// (Sheet::rework), so a batch of it is `{add, remove}` like an eraser's, and
// is taken the same way by every computer. Everything is in ink units.
namespace Ink {

// Whether the path, closed, goes around the stroke: around most of its points.
[[nodiscard]] bool lassoed(const Stroke &stroke, const QPolygonF &path);

class Lasso {
public:
    explicit Lasso(Sheet &sheet) : m_sheet(&sheet) {}

    // A pointer down at `at`: on the selection's box, or within `grab` of
    // it, it takes the selection to drag; anywhere else it begins a path.
    void press(QPointF at, double grab);
    void drag(QPointF to);
    // The pointer up: a path selects what it goes around (no edit), a drag
    // moves the selection where it was dragged (one step).
    Change release(QPointF at);
    // The pointer was no lasso after all (a palm): nothing it did stays.
    void cancel();
    [[nodiscard]] bool busy() const { return m_gesture != Gesture::None; }

    [[nodiscard]] const Sheet &sheet() const { return *m_sheet; }
    [[nodiscard]] const QPolygonF &path() const { return m_path; }
    [[nodiscard]] const QStringList &selection() const { return m_selection; }
    // Around the selected strokes, where they are being dragged to; empty with none.
    [[nodiscard]] QRectF box() const;
    // A selected stroke where it is being dragged to, null when none is dragged.
    [[nodiscard]] const Stroke *moving(QStringView id) const;
    // The strokes drawn where they were dragged to, and no longer: to draw again where they are.
    [[nodiscard]] QStringList takeStale();
    // The selected strokes, in the order they lie.
    [[nodiscard]] std::vector<Stroke> selected() const;

    void select(QStringList ids);
    void selectAll();
    void deselect();

    // Edits of the selection, each one step, answering the change made; the selection follows its strokes.
    Change move(QPointF by);
    Change recolor(const QColor &color);
    Change remove();
    // These strokes on top of the others, moved by `by`, each with a new id: selected.
    Change paste(std::vector<Stroke> strokes, QPointF by);

    // After an edit of the sheet: a selected stroke it replaced is replaced
    // in the selection by what came of it (a stroke moved, or undone back).
    void follow(const Change &change);
    // After a change from elsewhere: what is no longer on the sheet leaves the selection.
    void prune();
    // Told whenever the selection, its box or the path changes.
    void onChange(std::function<void()> told) { m_told = std::move(told); }

private:
    enum class Gesture { None, Path, Drag };

    void changed() const;

    Sheet *m_sheet; // never null: the sheet the surface has
    QStringList m_selection;
    QPolygonF m_path;
    Gesture m_gesture = Gesture::None;
    QPointF m_from;
    QPointF m_offset;
    std::vector<Stroke> m_moving;
    QStringList m_stale;
    std::function<void()> m_told;
};

// What the lasso shows, as triangles in the coordinates it is shown in
// (`scale` of them to an ink unit): the path being drawn and the box around
// the selection, faintly filled and standing off it, in `color`, lines
// `line` of them wide.
void marks(const QPolygonF &path, QRectF box, QColor color, double scale, double line, Mesh &mesh);

} // namespace Ink
