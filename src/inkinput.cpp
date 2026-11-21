#include "inkinput.h"

#include <QPointingDevice>

namespace Ink {

namespace {

bool within(quint64 since, quint64 now, quint64 grace) { return now >= since ? now - since < grace : true; }

bool &used() {
    static bool pen = false;
    return pen;
}

} // namespace

void InputRules::penEvent(quint64 ms) {
    used() = true;
    m_penSeen = true;
    m_penHeard = true;
    m_lastPenMs = ms;
}

void InputRules::proximity(bool near, quint64 ms) {
    used() = true;
    m_penSeen = true;
    m_proximitySeen = true;
    m_inProximity = near;
    if (!near) {
        m_leftMs = ms;
    }
}

bool InputRules::near(quint64 ms) const {
    if (m_proximitySeen) {
        return m_inProximity || within(m_leftMs, ms, limits.penGraceMs);
    }
    return m_penHeard && within(m_lastPenMs, ms, limits.penGraceMs);
}

Act InputRules::finger(double contactMm, quint64 ms, Fingers fingers) const {
    if (contactMm > limits.largestFingerMm || near(ms)) {
        return Act::Palm;
    }
    switch (fingers) {
    case Fingers::Draw:
        return Act::Draw;
    case Fingers::Scroll:
        return Act::Pass;
    case Fingers::Auto:
        break;
    }
    return m_penSeen ? Act::Pass : Act::Draw;
}

qint64 deviceId(const QPointingDevice *device) { return device != nullptr ? device->systemId() : -1; }

bool penUsed() { return used(); }

bool can(const QPointingDevice *device, QInputDevice::Capability capability) {
    return device != nullptr && device->capabilities().testFlag(capability);
}

} // namespace Ink
