#include "scriptwatchdog.h"

#include <algorithm>
#include <utility>

namespace {
// The wall-clock cap, as a multiple of the limit: a minute for the usual five
// seconds. A script that does its five seconds of work with a twelfth of a
// processor, as on a machine loaded far past its cores, still finishes.
constexpr int capTimes = 12;
} // namespace

ScriptWatchdog::ScriptWatchdog(QJSEngine *engine, std::function<void()> fired)
    : m_engine(engine), m_fired(std::move(fired)), m_thread([this] { watch(); }) {}

ScriptWatchdog::~ScriptWatchdog() {
    {
        const std::scoped_lock lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void ScriptWatchdog::arm(std::chrono::milliseconds limit) {
    {
        const std::scoped_lock lock(m_mutex);
        m_armed = true;
        m_worked = ThreadClock();
        m_limit = limit;
        m_cap = std::chrono::steady_clock::now() + limit * capTimes;
    }
    m_wake.notify_all();
}

void ScriptWatchdog::disarm() {
    {
        const std::scoped_lock lock(m_mutex);
        m_armed = false;
    }
    m_wake.notify_all();
}

void ScriptWatchdog::watch() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_quit) {
        if (!m_armed) {
            m_wake.wait(lock);
            continue;
        }
        const std::chrono::duration<double, std::milli> worked(m_worked.milliseconds());
        const auto now = std::chrono::steady_clock::now();
        if (worked < m_limit && now < m_cap) {
            // A thread works no faster than the wall clock goes, so the rest
            // of its limit is the soonest it can be used up. Woken early means
            // disarmed, armed anew, or quitting: look again.
            const auto rest = std::chrono::duration_cast<std::chrono::steady_clock::duration>(m_limit - worked);
            m_wake.wait_until(lock, std::min(now + rest, m_cap));
            continue;
        }
        m_armed = false;
        m_engine->setInterrupted(true);
        m_fired();
    }
}
