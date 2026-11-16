#include "hlc.h"

#include <QDateTime>
#include <algorithm>
#include <utility>

Hlc::Hlc(qint64 last, WallClock clock)
    : m_last(normalize(last)), m_clock(clock ? std::move(clock) : WallClock([] { return QDateTime::currentSecsSinceEpoch(); })) {}

qint64 Hlc::tick() { return tick(m_clock()); }

qint64 Hlc::tick(qint64 wallSeconds) noexcept {
    const qint64 wall = std::max<qint64>(1, wallSeconds);
    const qint64 candidate = pack(wall, 0);
    // When the wall clock has moved past the last timestamp's second, the new
    // second starts at counter 0; otherwise incrementing the packed value
    // advances the counter (and carries into the second if 65535 is passed).
    m_last = candidate > m_last ? candidate : m_last + 1;
    return m_last;
}

void Hlc::observe(qint64 remote) noexcept {
    if (remote > 0)
        m_last = std::max(m_last, normalize(remote));
}

qint64 Hlc::receive(qint64 remote) {
    observe(remote);
    return tick();
}

qint64 Hlc::receive(qint64 remote, qint64 wallSeconds) noexcept {
    observe(remote);
    return tick(wallSeconds);
}
