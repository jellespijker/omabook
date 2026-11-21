// What a touch does on an ink surface (InkSurface): a finger writes or erases,
// a palm is ignored, a finger that scrolls is passed on, and two fingers
// where fingers write move what is under the surface (#134).

#include <QLineF>
#include <QPointingDevice>
#include <QTouchEvent>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "inksurface.h"

namespace {

qint64 deviceId(const QPointingDevice *device) { return device != nullptr ? device->systemId() : -1; }

Ink::Pointer pointerOf(const QTouchEvent &event, const QEventPoint &point) { return {deviceId(event.pointingDevice()), point.id()}; }

Ink::Fingers fingersOf(InkSurface::FingerUse use) {
    switch (use) {
    case InkSurface::FingerUse::Draw:
        return Ink::Fingers::Draw;
    case InkSurface::FingerUse::Scroll:
        return Ink::Fingers::Scroll;
    case InkSurface::FingerUse::Auto:
        break;
    }
    return Ink::Fingers::Auto;
}

// Moves a Flickable's content by `step` along one axis, within its bounds.
void moveAlong(QQuickItem &flickable, const char *content, const char *origin, const char *extent, double room, double step) {
    const double at = flickable.property(content).toDouble();
    const double first = flickable.property(origin).toDouble();
    const double last = first + std::max(0.0, flickable.property(extent).toDouble() - room);
    const double to = std::clamp(at - step, first, std::max(first, last));
    if (std::abs(to - at) > 1e-9)
        flickable.setProperty(content, to);
}

} // namespace

void InkSurface::touchEvent(QTouchEvent *event) {
    if (event->type() == QEvent::TouchCancel) {
        dropTouches();
        return;
    }
    if (m_readOnly) {
        event->ignore();
        return;
    }
    if (m_gesture.isEmpty() && twoFingers(*event))
        beginGesture(*event);
    if (!m_gesture.isEmpty()) {
        gesture(*event);
        m_touchedAt = event->timestamp();
        return;
    }
    bool taken = false;
    for (qsizetype i = 0; i < event->pointCount(); ++i) {
        QEventPoint &point = event->point(i);
        touchPoint(*event, point);
        if (point.isAccepted() && point.state() == QEventPoint::State::Pressed) {
            event->setExclusiveGrabber(point, this);
        }
        taken = taken || point.isAccepted();
    }
    event->setAccepted(taken);
    if (taken) {
        m_touchedAt = event->timestamp();
    }
}

Ink::Act InkSurface::actOf(const QTouchEvent &event, const QEventPoint &point) const {
    const QSizeF contact = point.ellipseDiameters();
    const double contactMm = std::max(contact.width(), contact.height()) * millimetresPerPixel();
    return m_writing.toolDown() ? Ink::Act::Palm : m_rules.finger(contactMm, event.timestamp(), fingersOf(m_fingers));
}

void InkSurface::touchPoint(const QTouchEvent &event, QEventPoint &point) {
    const Ink::Pointer pointer = pointerOf(event, point);
    const quint64 ms = event.timestamp();
    const bool released = point.state() == QEventPoint::State::Released;
    if (point.state() == QEventPoint::State::Pressed) {
        const Ink::Act act = actOf(event, point);
        if (act == Ink::Act::Draw) {
            begin(pointer, inputAt(point.position(), ms), m_tool == Tool::Eraser, true);
        } else if (act == Ink::Act::Palm) {
            m_palms.insert(pointer);
        }
        point.setAccepted(act != Ink::Act::Pass);
        return;
    }
    if (m_palms.contains(pointer)) {
        if (released) {
            m_palms.remove(pointer);
        }
        point.setAccepted(true);
        return;
    }
    if (!m_writing.has(pointer)) {
        point.setAccepted(false);
        return;
    }
    point.setAccepted(true);
    // A palm that landed as a fingertip, and spread, or the pen coming near
    // while it was down: it was never a stroke.
    const QSizeF contact = point.ellipseDiameters();
    const double contactMm = std::max(contact.width(), contact.height()) * millimetresPerPixel();
    if (m_rules.finger(contactMm, ms, Ink::Fingers::Draw) == Ink::Act::Palm) {
        drop(pointer);
        if (!released) {
            m_palms.insert(pointer);
        }
    } else if (released) {
        lift(pointer, inputAt(point.position(), ms));
    } else if (point.state() != QEventPoint::State::Stationary) {
        edited(m_writing.move(pointer, inputAt(point.position(), ms)));
        update();
    }
}

void InkSurface::touchUngrabEvent() { dropTouches(); }

bool InkSurface::twoFingers(const QTouchEvent &event) const {
    bool fresh = false;
    qsizetype fingers = 0;
    for (const QEventPoint &point : event.points()) {
        if (point.state() == QEventPoint::State::Pressed) {
            if (actOf(event, point) == Ink::Act::Draw) {
                fresh = true;
                ++fingers;
            }
        } else if (point.state() != QEventPoint::State::Released && m_writing.has(pointerOf(event, point))) {
            ++fingers;
        }
    }
    return fresh && fingers > 1;
}

void InkSurface::beginGesture(const QTouchEvent &event) {
    // What a finger began writing was the gesture's start: it goes, and the
    // finger is one of the gesture's (the fingers just down join it in `gesture`).
    const std::vector<Ink::Pointer> dropped = m_writing.dropTouches();
    for (const QEventPoint &point : event.points()) {
        const Ink::Pointer pointer = pointerOf(event, point);
        if (std::ranges::contains(dropped, pointer)) {
            m_gesture.insert(pointer, point.scenePosition());
        }
    }
    if (!dropped.empty()) {
        m_restructured = true;
        if (m_writing.empty()) {
            emit writingChanged();
        }
        update();
    }
}

void InkSurface::gesture(QTouchEvent &event) {
    std::vector<std::pair<QPointF, QPointF>> moved;
    for (qsizetype i = 0; i < event.pointCount(); ++i) {
        QEventPoint &point = event.point(i);
        const Ink::Pointer pointer = pointerOf(event, point);
        point.setAccepted(true);
        if (point.state() == QEventPoint::State::Pressed) {
            event.setExclusiveGrabber(point, this);
            if (actOf(event, point) == Ink::Act::Palm) {
                m_palms.insert(pointer);
            } else {
                m_gesture.insert(pointer, point.scenePosition());
            }
            continue;
        }
        const bool released = point.state() == QEventPoint::State::Released;
        if (m_palms.contains(pointer) && released) {
            m_palms.remove(pointer);
        }
        const auto found = m_gesture.find(pointer);
        if (found == m_gesture.end()) {
            continue;
        }
        moved.emplace_back(*found, point.scenePosition());
        if (released) {
            m_gesture.erase(found);
        } else {
            *found = point.scenePosition();
        }
    }
    event.setAccepted(true);
    follow(moved);
}

void InkSurface::follow(const std::vector<std::pair<QPointF, QPointF>> &moved) {
    if (moved.empty()) {
        return;
    }
    QPointF from;
    QPointF to;
    for (const auto &[before, after] : moved) {
        from += before;
        to += after;
    }
    const auto count = static_cast<double>(moved.size());
    from /= count;
    to /= count;
    moveUnder(to - from);
    if (moved.size() < 2) {
        return;
    }
    double spanBefore = 0.0;
    double spanAfter = 0.0;
    for (const auto &[before, after] : moved) {
        spanBefore += QLineF(from, before).length();
        spanAfter += QLineF(to, after).length();
    }
    if (spanBefore > 1.0 && spanAfter > 1.0 && std::abs(spanAfter - spanBefore) > 1e-6) {
        emit pinched(spanAfter / spanBefore);
    }
}

void InkSurface::moveUnder(QPointF by) {
    QQuickItem *under = parentItem();
    while (under != nullptr && !under->inherits("QQuickFlickable")) {
        under = under->parentItem();
    }
    // One that holds still (pages that turn) isn't moved by hand.
    if (under == nullptr || !under->property("interactive").toBool()) {
        return;
    }
    moveAlong(*under, "contentX", "originX", "contentWidth", under->width(), by.x());
    moveAlong(*under, "contentY", "originY", "contentHeight", under->height(), by.y());
}
