#pragma once

#include <QJSEngine>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "threadclock.h"

// Stops a script that doesn't come back. JavaScript runs on the thread that
// called it, so a timer there would never fire; this one waits on a thread
// of its own and interrupts the engine, which QJSEngine allows from anywhere.
//
// The limit is the script thread's CPU time, not the wall clock's: on a busy
// machine the thread also waits for a processor, and that is not the script
// running (#139). A much later wall-clock cap stays as a last resort, for a
// script that holds its thread without working, in a call that waits on
// something that never comes: it is stopped once that call returns.
class ScriptWatchdog {
public:
    // `fired` is called on the watchdog's thread once the engine was interrupted.
    ScriptWatchdog(QJSEngine *engine, std::function<void()> fired);
    ~ScriptWatchdog();
    ScriptWatchdog(const ScriptWatchdog &) = delete;
    ScriptWatchdog &operator=(const ScriptWatchdog &) = delete;
    ScriptWatchdog(ScriptWatchdog &&) = delete;
    ScriptWatchdog &operator=(ScriptWatchdog &&) = delete;

    // Called on the script's thread: its CPU time from here on counts.
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
    ThreadClock m_worked; // Made anew on the script's thread at each arm().
    std::chrono::milliseconds m_limit{};
    std::chrono::steady_clock::time_point m_cap;
    std::thread m_thread;
};
