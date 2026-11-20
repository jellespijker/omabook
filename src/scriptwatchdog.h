#pragma once

#include <QJSEngine>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// Stops a script that doesn't come back. JavaScript runs on the thread that
// called it, so a timer there would never fire; this one waits on a thread
// of its own and interrupts the engine, which QJSEngine allows from anywhere.
class ScriptWatchdog {
public:
    // `fired` is called on the watchdog's thread once the engine was interrupted.
    ScriptWatchdog(QJSEngine *engine, std::function<void()> fired);
    ~ScriptWatchdog();
    ScriptWatchdog(const ScriptWatchdog &) = delete;
    ScriptWatchdog &operator=(const ScriptWatchdog &) = delete;
    ScriptWatchdog(ScriptWatchdog &&) = delete;
    ScriptWatchdog &operator=(ScriptWatchdog &&) = delete;

    void arm(std::chrono::milliseconds limit);
    void disarm();

private:
    void watch();

    QJSEngine *m_engine;
    std::function<void()> m_fired;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_armed = false;
    bool m_quit = false;
    std::chrono::steady_clock::time_point m_deadline;
    std::thread m_thread;
};
