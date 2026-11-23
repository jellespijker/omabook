#include "hubautorun.h"

#include <QLockFile>
#include <utility>

HubAutoRun::HubAutoRun(Given given, QObject *parent) : QObject(parent), m_given(std::move(given)), m_auto(m_given.folder) {
    m_timer.setInterval(m_given.every);
    connect(&m_timer, &QTimer::timeout, this, &HubAutoRun::tick);
}

void HubAutoRun::start() {
    SyncWait::whenOnline(this, [this] {
        m_auto.cameIntoReach();
        tick();
    });
    m_timer.start();
    tick();
}

void HubAutoRun::tick() {
    const QDateTime now = m_given.now();
    const auto host = m_auto.due(m_given.hosts(), now);
    if (!host || m_given.wait() != SyncWait::Reason::None)
        return;
    // The window, or the person in a terminal, is syncing: the next look.
    const auto held = m_auto.hold();
    if (!held)
        return;
    m_auto.began(now);
    const SyncSummary summary = m_given.sync(*host);
    if (const auto tell = m_auto.done(*host, summary, m_given.now()); tell && m_given.notify) {
        const auto [title, body] = HubAutoSync::notice(*host, *tell);
        m_given.notify(title, body);
    }
}
