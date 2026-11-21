#pragma once

#include <ctime>

// How long the calling thread has worked since the clock was made, in
// milliseconds: its CPU time, not the time on the wall. What a frame costs to
// put together is the work it does; on a busy machine the wall clock also
// counts the time the thread waited for a processor, which says nothing about
// the drawing (#132).
class ThreadClock {
public:
    ThreadClock() noexcept : m_start(now()) {}

    [[nodiscard]] double milliseconds() const noexcept { return now() - m_start; }

private:
    [[nodiscard]] static double now() noexcept {
        timespec at{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &at);
        return static_cast<double>(at.tv_sec) * 1e3 + static_cast<double>(at.tv_nsec) / 1e6;
    }

    double m_start;
};
