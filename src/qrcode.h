#pragma once

#include <QByteArray>
#include <optional>
#include <vector>

// A QR code (ISO/IEC 18004) of some bytes, for a phone's camera to read an
// address off the screen (#177): byte mode, error correction level M (a
// smudge or a reflection over 15% of it is read through), the smallest of
// versions 1 to 10 the bytes fit in, and the mask the standard's penalty
// rules choose. Version 10 holds 213 bytes, more than an address with a
// token in it needs. It is the modules only; whoever shows it draws them,
// with the four modules of light margin a reader looks for around them.
class QrCode {
public:
    static constexpr int largestVersion = 10;

    // The code of `bytes`; none when there are too many.
    [[nodiscard]] static std::optional<QrCode> of(const QByteArray &bytes);

    // Modules on a side: 21 for version 1, four more for every version after.
    [[nodiscard]] int size() const noexcept { return m_size; }
    [[nodiscard]] int version() const noexcept { return m_version; }
    [[nodiscard]] int mask() const noexcept { return m_mask; }
    // Whether the module in column `x` of row `y` is dark; outside it, light.
    [[nodiscard]] bool dark(int x, int y) const;

private:
    QrCode(int version, int mask, std::vector<bool> modules);

    int m_version = 1;
    int m_size = 21;
    int m_mask = 0;
    std::vector<bool> m_modules; // row by row
};
