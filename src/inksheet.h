#pragma once

#include <QPointF>
#include <QStringView>

#include <optional>
#include <vector>

#include "inkstroke.h"

namespace Ink {

enum class Erasing {
    Parts,   // what the eraser goes over, cutting a stroke in pieces
    Strokes, // every stroke it touches, whole
};

// The strokes on one ink surface, one over the other in the order they
// began, and what was done to them, to undo. Each edit answers the change it
// made, to be kept wherever the strokes are kept (#78). What comes from
// there (another computer's strokes, the ones read when the surface opens)
// is applied without being an edit, and is not undone.
class Sheet {
public:
    [[nodiscard]] const std::vector<Stroke> &strokes() const { return m_strokes; }
    [[nodiscard]] const Stroke *find(QStringView id) const;

    // Edits: each a step of undo of its own, or a part of the one begun.
    Change add(Stroke stroke);
    Change erase(QPointF centre, double radius, Erasing how);
    Change clear();
    // The edits until endStep() as one step of undo: an eraser's sweep.
    void beginStep();
    void endStep();
    // Each answers the change it made (none when there was nothing to undo or redo).
    Change undo();
    Change redo();
    [[nodiscard]] bool canUndo() const { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const { return !m_redo.empty(); }

    // What comes from where the strokes are kept.
    void apply(const Change &change);
    void replace(std::vector<Stroke> strokes);

private:
    struct Step {
        std::vector<Stroke> added;
        std::vector<Stroke> removed;
    };

    // Removes and adds, and answers the strokes it removed.
    std::vector<Stroke> perform(const QStringList &removed, std::vector<Stroke> added);
    void insert(Stroke stroke);
    void record(Step step);
    static Change changeOf(const Step &step);

    std::vector<Stroke> m_strokes;
    std::vector<Step> m_undo;
    std::vector<Step> m_redo;
    std::optional<Step> m_open;
};

} // namespace Ink
