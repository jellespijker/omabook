#include "inksheet.h"

#include <algorithm>
#include <iterator>
#include <tuple>
#include <utility>

namespace Ink {

namespace {

// Steps of undo kept: enough for any page's work, bounded for a long session.
constexpr std::size_t undoDepth = 200;

QStringList idsOf(const std::vector<Stroke> &strokes) {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(strokes.size()));
    for (const Stroke &stroke : strokes) {
        ids.append(stroke.id);
    }
    return ids;
}

bool before(const Stroke &a, const Stroke &b) { return std::tie(a.began, a.id) < std::tie(b.began, b.id); }

} // namespace

const Stroke *Sheet::find(QStringView id) const {
    const auto found = std::ranges::find(m_strokes, id, &Stroke::id);
    return found != m_strokes.end() ? &*found : nullptr;
}

Change Sheet::add(Stroke stroke) {
    std::ignore = perform({}, {stroke});
    Step step{.added = {std::move(stroke)}, .removed = {}};
    Change change = changeOf(step);
    record(std::move(step));
    return change;
}

Change Sheet::erase(QPointF centre, double radius, Erasing how) {
    QStringList gone;
    std::vector<Stroke> pieces;
    const QRectF reach(centre - QPointF(radius, radius), centre + QPointF(radius, radius));
    for (const Stroke &stroke : m_strokes) {
        if (!bounds(stroke).intersects(reach)) {
            continue;
        }
        if (how == Erasing::Strokes) {
            if (touches(stroke, centre, radius)) {
                gone.append(stroke.id);
            }
        } else if (auto left = cut(stroke, centre, radius)) {
            gone.append(stroke.id);
            std::ranges::move(*left, std::back_inserter(pieces));
        }
    }
    if (gone.isEmpty()) {
        return {};
    }
    Step step{.added = pieces, .removed = perform(gone, pieces)};
    Change change = changeOf(step);
    record(std::move(step));
    return change;
}

Change Sheet::clear() {
    if (m_strokes.empty()) {
        return {};
    }
    Step step{.added = {}, .removed = perform(idsOf(m_strokes), {})};
    Change change = changeOf(step);
    record(std::move(step));
    return change;
}

Change Sheet::addAll(std::vector<Stroke> strokes) {
    if (strokes.empty()) {
        return {};
    }
    std::ignore = perform({}, strokes);
    Step step{.added = std::move(strokes), .removed = {}};
    Change change = changeOf(step);
    record(std::move(step));
    return change;
}

Change Sheet::remove(const QStringList &ids) {
    std::vector<Stroke> gone = perform(ids, {});
    if (gone.empty()) {
        return {};
    }
    Step step{.added = {}, .removed = std::move(gone)};
    Change change = changeOf(step);
    record(std::move(step));
    return change;
}

Change Sheet::rework(const QStringList &ids, const std::function<void(Stroke &)> &how) {
    std::vector<Stroke> made;
    QStringList found;
    for (const QString &id : ids) {
        if (const Stroke *stroke = find(id)) {
            Stroke copy = *stroke;
            copy.id = newId();
            how(copy);
            made.push_back(std::move(copy));
            found.append(id);
        }
    }
    if (made.empty()) {
        return {};
    }
    Step step{.added = made, .removed = perform(found, made)};
    Change change = changeOf(step);
    record(std::move(step));
    return change;
}

void Sheet::beginStep() {
    if (!m_open) {
        m_open = Step{};
    }
}

void Sheet::endStep() {
    if (!m_open) {
        return;
    }
    Step step = std::move(*m_open);
    m_open.reset();
    if (!step.added.empty() || !step.removed.empty()) {
        record(std::move(step));
    }
}

Change Sheet::undo() {
    endStep();
    if (m_undo.empty()) {
        return {};
    }
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    std::ignore = perform(idsOf(step.added), step.removed);
    Change change{.added = step.removed, .removed = idsOf(step.added)};
    m_redo.push_back(std::move(step));
    return change;
}

Change Sheet::redo() {
    endStep();
    if (m_redo.empty()) {
        return {};
    }
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    std::ignore = perform(idsOf(step.removed), step.added);
    Change change = changeOf(step);
    m_undo.push_back(std::move(step));
    return change;
}

void Sheet::apply(const Change &change) { std::ignore = perform(change.removed, change.added); }

void Sheet::replace(std::vector<Stroke> strokes) {
    m_strokes = std::move(strokes);
    std::ranges::sort(m_strokes, before);
    m_undo.clear();
    m_redo.clear();
    m_open.reset();
}

std::vector<Stroke> Sheet::perform(const QStringList &removed, std::vector<Stroke> added) {
    std::vector<Stroke> taken;
    for (const QString &id : removed) {
        const auto found = std::ranges::find(m_strokes, id, &Stroke::id);
        if (found != m_strokes.end()) {
            taken.push_back(std::move(*found));
            m_strokes.erase(found);
        }
    }
    for (Stroke &stroke : added) {
        insert(std::move(stroke));
    }
    return taken;
}

void Sheet::insert(Stroke stroke) {
    const auto same = std::ranges::find(m_strokes, stroke.id, &Stroke::id);
    if (same != m_strokes.end()) {
        m_strokes.erase(same);
    }
    const auto at = std::ranges::upper_bound(m_strokes, stroke, before);
    m_strokes.insert(at, std::move(stroke));
}

void Sheet::record(Step step) {
    m_redo.clear();
    if (!m_open) {
        m_undo.push_back(std::move(step));
        if (m_undo.size() > undoDepth) {
            m_undo.erase(m_undo.begin());
        }
        return;
    }
    // Within a step, a stroke made and erased again was never there.
    for (Stroke &stroke : step.removed) {
        const auto made = std::ranges::find(m_open->added, stroke.id, &Stroke::id);
        if (made != m_open->added.end()) {
            m_open->added.erase(made);
        } else {
            m_open->removed.push_back(std::move(stroke));
        }
    }
    std::ranges::move(step.added, std::back_inserter(m_open->added));
}

Change Sheet::changeOf(const Step &step) { return Change{.added = step.added, .removed = idsOf(step.removed)}; }

} // namespace Ink
