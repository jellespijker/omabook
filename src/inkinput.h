#pragma once

#include <QInputDevice>
#include <QtTypes>

class QPointingDevice;

// Which touches of an ink surface are a palm (#78). A pen always writes. A
// touch is a palm, ignored, when it is wider than a fingertip or comes while
// the pen is near the screen; what Qt says of the pen decides how "near" is
// known, so that each thing a device may not deliver falls back to the next:
//
//   - with proximity events, near is from the pen entering proximity until
//     a moment after it leaves;
//   - without them, near is a moment after any pen event, hovering included;
//   - without a contact size, a palm is told by nearness alone.
//
// A finger that is no palm draws until a pen has been seen, and scrolls
// after (`Fingers::Auto`), unless the surface says always one or the other.
namespace Ink {

enum class Fingers { Auto, Draw, Scroll };

// What a touch does: draws (or erases, with the eraser chosen), is a palm
// and ignored, or is passed on to what is under the surface, to scroll.
enum class Act { Draw, Palm, Pass };

struct PalmLimits {
    // A contact wider than this is a palm (a fingertip is 7 to 10 mm).
    double largestFingerMm = 12.0;
    // How long after the pen was near a touch is still taken for a palm.
    quint64 penGraceMs = 500;
};

class InputRules {
public:
    // `penKnown`: Qt already knows a pen on this computer, before any of its events.
    explicit InputRules(bool penKnown = false) : m_penSeen(penKnown) {}

    // Times are the events' own, in milliseconds.
    void penEvent(quint64 ms);
    void proximity(bool near, quint64 ms);
    [[nodiscard]] Act finger(double contactMm, quint64 ms, Fingers fingers) const;

    [[nodiscard]] bool penSeen() const { return m_penSeen; }
    [[nodiscard]] bool penNear() const { return m_inProximity; }
    [[nodiscard]] bool proximityKnown() const { return m_proximitySeen; }

    PalmLimits limits;

private:
    [[nodiscard]] bool near(quint64 ms) const;

    bool m_penSeen = false;
    bool m_penHeard = false;
    quint64 m_lastPenMs = 0;
    bool m_proximitySeen = false;
    bool m_inProximity = false;
    quint64 m_leftMs = 0;
};

// What Qt says of the devices: a pointing device's id (-1 for none),
// whether it knows a pen on this computer, and what a device can tell.
[[nodiscard]] qint64 deviceId(const QPointingDevice *device);
[[nodiscard]] bool knowsAPen();
[[nodiscard]] bool can(const QPointingDevice *device, QInputDevice::Capability capability);

} // namespace Ink
