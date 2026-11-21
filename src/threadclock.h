#pragma once

#include <ctime>
#include <pthread.h>

// How long a thread has worked since the clock was made on it, in
// milliseconds: its CPU time, not the time on the wall. What a frame costs to
// put together is the work it does; on a busy machine the wall clock also
// counts the time the thread waited for a processor, which says nothing about
// the drawing (#132). The clock measures the thread that made it, and may be
// read from any thread while that one lives: the script watchdog reads the
// script's thread from its own (#139).
class ThreadClock {
public:
    ThreadClock() noexcept : m_start(now()) {}

    [[nodiscard]] double milliseconds() const noexcept { return now() - m_start; }

private:
    [[nodiscard]] static clockid_t own() noexcept {
        clockid_t clock = CLOCK_THREAD_CPUTIME_ID;
        pthread_getcpuclockid(pthread_self(), &clock);
        return clock;
    }

    [[nodiscard]] double now() const noexcept {
        timespec at{};
        clock_gettime(m_clock, &at);
        return static_cast<double>(at.tv_sec) * 1e3 + static_cast<double>(at.tv_nsec) / 1e6;
    }

    clockid_t m_clock = own();
    double m_start;
};
