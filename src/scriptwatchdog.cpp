#include "scriptwatchdog.h"

#include <utility>

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
        m_deadline = std::chrono::steady_clock::now() + limit;
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
        // Woken early means disarmed, armed anew, or quitting: look again.
        if (m_wake.wait_until(lock, m_deadline) != std::cv_status::timeout || !m_armed || std::chrono::steady_clock::now() < m_deadline)
            continue;
        m_armed = false;
        m_engine->setInterrupted(true);
        m_fired();
    }
}
