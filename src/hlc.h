#pragma once

#include <QtTypes>
#include <compare>
#include <functional>

// A hybrid logical clock (ADR 0001): wall-clock seconds in the upper bits
// and a 16-bit counter in the lower bits of a qint64, so writes order in the
// order they were made even when two computers' clocks are minutes apart or
// several writes happen in the same second.
//
// Zero stays zero ("no time known", as for book details from before times
// were kept), and plain Unix-second timestamps from older libraries (< 2^32)
// are read as that wall time with counter 0. Because a packed clock uses 47
// bits for current dates, it round-trips intact through SQLite INTEGER and
// JSON numbers (IEEE 754 double, 53-bit significand).
class Hlc {
public:
    using WallClock = std::function<qint64()>;

    static constexpr unsigned int counterBits = 16U;
    static constexpr quint64 counterMask = (quint64{1} << counterBits) - quint64{1};
    // Plain Unix seconds fit below 2^32 until 2106; any packed clock after
    // 1970-01-02 lies at or above 2^32.
    static constexpr qint64 packedFloor = static_cast<qint64>(quint64{1} << 32U);

    explicit Hlc(qint64 last = 0, WallClock clock = {});

    // Packs a wall-clock second and a logical counter into one qint64.
    [[nodiscard]] static constexpr qint64 pack(qint64 seconds, int counter = 0) noexcept {
        if (seconds <= 0 && counter <= 0)
            return 0;
        const quint64 wall = seconds > 0 ? static_cast<quint64>(seconds) : quint64{0};
        const quint64 count = counter > 0 ? (static_cast<quint64>(counter) & counterMask) : quint64{0};
        return static_cast<qint64>((wall << counterBits) | count);
    }

    [[nodiscard]] static constexpr bool isPacked(qint64 value) noexcept { return value >= packedFloor; }

    // Turns a plain-seconds timestamp from an older library into a packed HLC
    // with counter 0, leaving 0 and already-packed values as they are.
    [[nodiscard]] static constexpr qint64 normalize(qint64 value) noexcept {
        if (value <= 0)
            return 0;
        return isPacked(value) ? value : pack(value, 0);
    }

    // The wall-clock seconds of a timestamp, whether packed or plain seconds.
    [[nodiscard]] static constexpr qint64 seconds(qint64 value) noexcept {
        if (value <= 0)
            return 0;
        return isPacked(value) ? static_cast<qint64>(static_cast<quint64>(value) >> counterBits) : value;
    }

    // The logical counter within that second (0 for a plain-seconds value).
    [[nodiscard]] static constexpr int counter(qint64 value) noexcept {
        return isPacked(value) ? static_cast<int>(static_cast<quint64>(value) & counterMask) : 0;
    }

    // Orders two timestamps on the HLC scale, whether either is plain seconds
    // or already packed.
    [[nodiscard]] static constexpr std::strong_ordering compare(qint64 a, qint64 b) noexcept { return normalize(a) <=> normalize(b); }

    [[nodiscard]] qint64 last() const noexcept { return m_last; }

    // Issues a timestamp strictly after every timestamp this clock has issued
    // or observed so far.
    [[nodiscard]] qint64 tick();
    [[nodiscard]] qint64 tick(qint64 wallSeconds) noexcept;

    // Remembers a timestamp seen from another computer (or already in the
    // database), so the next tick() comes after it even if this computer's
    // wall clock is behind.
    void observe(qint64 remote) noexcept;

    // Observes `remote` and immediately issues the next timestamp after it.
    [[nodiscard]] qint64 receive(qint64 remote);
    [[nodiscard]] qint64 receive(qint64 remote, qint64 wallSeconds) noexcept;

private:
    qint64 m_last = 0;
    WallClock m_clock;
};
