#include "inkwriting.h"

#include <QDateTime>
#include <QLineF>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace Ink {

namespace {

// A move less than this (an eighth of what is kept), at the same pressure, adds nothing to a stroke.
constexpr double stillness = 1.0 / 16.0;

Point pointOf(const Input &input, quint32 ms) {
    return Point{.at = input.at, .pressure = input.pressure, .xTilt = input.xTilt, .yTilt = input.yTilt, .ms = ms};
}

} // namespace

void Writing::write(Pointer pointer, const Input &input, const Nib &nib, bool touch) {
    if (m_live.contains(pointer)) {
        std::ignore = lift(pointer, input);
    }
    Live live{.stroke = Stroke{.id = newId(),
                               .color = nib.color,
                               .width = nib.width,
                               .pressured = nib.pressured,
                               .tilted = nib.tilted,
                               .began = QDateTime::currentMSecsSinceEpoch(),
                               .points = {pointOf(input, 0)}},
              .erasing = false,
              .touch = touch,
              .startMs = input.ms,
              .last = input.at,
              .rubber = {},
              .lassoing = false};
    m_live.insert_or_assign(pointer, std::move(live));
}

Change Writing::erase(Pointer pointer, const Input &input, Rubber rubber, bool touch) {
    Change change;
    if (m_live.contains(pointer)) {
        change = lift(pointer, input);
    }
    m_sheet->beginStep();
    Live live{.stroke = {}, .erasing = true, .touch = touch, .startMs = input.ms, .last = input.at, .rubber = rubber, .lassoing = false};
    change.merge(m_sheet->erase(input.at, rubber.radius, rubber.how));
    m_live.insert_or_assign(pointer, std::move(live));
    return change;
}

bool Writing::lasso(Pointer pointer, const Input &input, double grab, bool touch) {
    if (m_lasso == nullptr || m_lasso->busy()) {
        return false;
    }
    m_lasso->press(input.at, grab);
    Live live{.stroke = {}, .erasing = false, .touch = touch, .startMs = input.ms, .last = input.at, .rubber = {}, .lassoing = true};
    m_live.insert_or_assign(pointer, std::move(live));
    return true;
}

Change Writing::move(Pointer pointer, const Input &input) {
    const auto live = m_live.find(pointer);
    if (live != m_live.end() && live->second.lassoing) {
        m_lasso->drag(input.at);
        return {};
    }
    return live != m_live.end() ? extend(live->second, input) : Change{};
}

Change Writing::lift(Pointer pointer, const Input &input) {
    const auto found = m_live.find(pointer);
    if (found == m_live.end()) {
        return {};
    }
    Live &live = found->second;
    Change change;
    if (live.lassoing) {
        change = m_lasso->release(input.at);
    } else if (live.erasing) {
        change = sweep(live, input.at);
        m_sheet->endStep();
    } else {
        // Lifting presses no harder or lighter: a pen says no pressure as it
        // lifts, and the line ends as it was, not thinner.
        Input last = input;
        last.pressure = live.stroke.points.back().pressure;
        change = extend(live, last);
        change.merge(m_sheet->add(std::move(live.stroke)));
    }
    m_live.erase(found);
    return change;
}

void Writing::drop(Pointer pointer) {
    const auto found = m_live.find(pointer);
    if (found == m_live.end()) {
        return;
    }
    if (found->second.erasing) {
        m_sheet->endStep();
    } else if (found->second.lassoing) {
        m_lasso->cancel();
    }
    m_live.erase(found);
}

std::vector<Pointer> Writing::dropTouches() {
    std::vector<Pointer> touches;
    for (const auto &[pointer, live] : m_live) {
        if (live.touch) {
            touches.push_back(pointer);
        }
    }
    for (const Pointer &pointer : touches) {
        drop(pointer);
    }
    return touches;
}

bool Writing::toolDown() const {
    return std::ranges::any_of(m_live, [](const auto &entry) { return !entry.second.touch; });
}

std::vector<const Stroke *> Writing::strokes() const {
    std::vector<const Stroke *> out;
    for (const auto &[pointer, live] : m_live) {
        if (!live.erasing && !live.lassoing) {
            out.push_back(&live.stroke);
        }
    }
    std::ranges::sort(out, {}, [](const Stroke *stroke) { return stroke->began; });
    return out;
}

Change Writing::extend(Live &live, const Input &input) {
    if (live.erasing) {
        return sweep(live, input.at);
    }
    std::vector<Point> &points = live.stroke.points;
    const Point point = pointOf(input, static_cast<quint32>(input.ms >= live.startMs ? input.ms - live.startMs : 0));
    if (QLineF(points.back().at, point.at).length() < stillness && std::abs(points.back().pressure - point.pressure) < 0.01F) {
        return {};
    }
    if (points.size() < maxPoints) {
        points.push_back(point);
        return {};
    }
    // Too long for one stroke: it goes on the sheet, and the line goes on as
    // the next, from where it got to.
    Point from = points.back();
    Stroke next{.id = newId(),
                .color = live.stroke.color,
                .width = live.stroke.width,
                .pressured = live.stroke.pressured,
                .tilted = live.stroke.tilted,
                .began = live.stroke.began + from.ms,
                .points = {}};
    live.startMs += from.ms;
    from.ms = 0;
    next.points.push_back(from);
    Change change = m_sheet->add(std::exchange(live.stroke, std::move(next)));
    change.merge(extend(live, input));
    return change;
}

Change Writing::sweep(Live &live, QPointF to) {
    // Every few steps along the way, so that a quick sweep leaves no gaps.
    const QPointF from = live.last;
    const double length = QLineF(from, to).length();
    const double spacing = std::max(live.rubber.radius / 2.0, 1e-3);
    const int steps = std::max(1, static_cast<int>(std::ceil(length / spacing)));
    Change change;
    for (int step = 1; step <= steps && length > 0.0; ++step) {
        change.merge(m_sheet->erase(from + (to - from) * (static_cast<double>(step) / steps), live.rubber.radius, live.rubber.how));
    }
    live.last = to;
    return change;
}

} // namespace Ink
