#include "spaceannotations.h"

using namespace Qt::StringLiterals;

bool SpaceAnnotations::travels(const QString &kind) { return kind == "highlight"_L1 || kind == "margin-note"_L1 || kind == "ink"_L1; }

SpaceAnnotations::Chosen SpaceAnnotations::choose(const ReducedState &state, const std::function<bool(const QString &bookGid)> &holds) {
    Chosen chosen;
    for (auto annotation = state.annotations.cbegin(); annotation != state.annotations.cend(); ++annotation) {
        if (!travels(annotation->fields.value(u"kind"_s).value) || !holds(annotation->fields.value(u"book"_s).value))
            continue;
        if (SpaceReducer::isRemoved(annotation->alive, annotation->removed))
            chosen.removed.insert(annotation.key());
        else
            chosen.alive.insert(annotation.key());
    }
    return chosen;
}

bool SpaceAnnotations::goes(const SpaceOperation &op, const Chosen &chosen) {
    if (op.entity != "annotation"_L1)
        return false;
    if (chosen.alive.contains(op.id))
        return true;
    return chosen.removed.contains(op.id) && op.field == "removed"_L1 && op.value == "1"_L1;
}
