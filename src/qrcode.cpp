#include "qrcode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace {

using Bytes = std::vector<std::uint8_t>;

// Level M's error correction, by version (from 1): the codewords of it each
// block has, and how many blocks the codewords are cut into.
constexpr std::array<int, QrCode::largestVersion> eccPerBlock{10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr std::array<int, QrCode::largestVersion> blockCount{1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
// Level M in the format information.
constexpr unsigned levelBits = 0b00U;

[[nodiscard]] int sizeOf(int version) { return 17 + 4 * version; }

// The modules that carry data and error correction, once every pattern is drawn.
[[nodiscard]] int rawModules(int version) {
    int modules = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int alignments = version / 7 + 2;
        modules -= (25 * alignments - 10) * alignments - 55;
        if (version >= 7)
            modules -= 36;
    }
    return modules;
}

[[nodiscard]] int dataCodewords(int version) {
    const auto at = static_cast<std::size_t>(version - 1);
    return rawModules(version) / 8 - eccPerBlock.at(at) * blockCount.at(at);
}

// The rows and columns the alignment patterns are centred on.
[[nodiscard]] std::vector<int> alignments(int version) {
    if (version == 1)
        return {};
    const int count = version / 7 + 2;
    const int step = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
    std::vector<int> centres(static_cast<std::size_t>(count), 6);
    for (int i = count - 1, at = sizeOf(version) - 7; i >= 1; --i, at -= step)
        centres.at(static_cast<std::size_t>(i)) = at;
    return centres;
}

// Multiplying in GF(2^8) over x^8 + x^4 + x^3 + x^2 + 1, the field of the code's Reed-Solomon.
[[nodiscard]] std::uint8_t times(std::uint8_t x, std::uint8_t y) {
    unsigned z = 0;
    for (int bit = 7; bit >= 0; --bit) {
        z = (z << 1U) ^ ((z >> 7U) * 0x11DU);
        z ^= ((static_cast<unsigned>(y) >> static_cast<unsigned>(bit)) & 1U) * static_cast<unsigned>(x);
    }
    return static_cast<std::uint8_t>(z);
}

// The generator polynomial of `degree` error correction codewords, leading term left out.
[[nodiscard]] Bytes divisor(int degree) {
    Bytes result(static_cast<std::size_t>(degree), 0);
    result.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (std::size_t j = 0; j < result.size(); ++j) {
            result.at(j) = times(result.at(j), root);
            if (j + 1 < result.size())
                result.at(j) ^= result.at(j + 1);
        }
        root = times(root, 0x02);
    }
    return result;
}

[[nodiscard]] Bytes remainder(const Bytes &data, const Bytes &by) {
    Bytes result(by.size(), 0);
    for (const std::uint8_t byte : data) {
        const auto factor = static_cast<std::uint8_t>(byte ^ result.front());
        std::ranges::rotate(result, result.begin() + 1);
        result.back() = 0;
        for (std::size_t i = 0; i < result.size(); ++i)
            result.at(i) ^= times(by.at(i), factor);
    }
    return result;
}

// The data codewords: byte mode, the count, the bytes, the terminator and the padding.
[[nodiscard]] Bytes encoded(const QByteArray &bytes, int version) {
    std::vector<bool> bits;
    const auto put = [&bits](unsigned value, int length) {
        for (int i = length - 1; i >= 0; --i)
            bits.push_back(((value >> static_cast<unsigned>(i)) & 1U) != 0);
    };
    put(0b0100, 4);
    put(static_cast<unsigned>(bytes.size()), version < 10 ? 8 : 16);
    for (const char byte : bytes)
        put(static_cast<std::uint8_t>(byte), 8);
    const auto capacity = static_cast<std::size_t>(dataCodewords(version)) * 8;
    put(0, static_cast<int>(std::min<std::size_t>(4, capacity - bits.size())));
    put(0, static_cast<int>((8 - bits.size() % 8) % 8));
    for (unsigned pad = 0xECU; bits.size() < capacity; pad ^= 0xECU ^ 0x11U)
        put(pad, 8);
    Bytes codewords(bits.size() / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        codewords.at(i / 8) |= static_cast<std::uint8_t>(bits.at(i) ? 0x80U >> (i % 8) : 0U);
    return codewords;
}

// The data cut into blocks, each with its error correction, interleaved.
[[nodiscard]] Bytes interleaved(const Bytes &data, int version) {
    const auto at = static_cast<std::size_t>(version - 1);
    const int count = blockCount.at(at);
    const int ecc = eccPerBlock.at(at);
    const int raw = rawModules(version) / 8;
    const int shortBlocks = count - raw % count;
    const int shortLength = raw / count;
    const Bytes by = divisor(ecc);
    std::vector<Bytes> blocks;
    auto from = data.begin();
    for (int i = 0; i < count; ++i) {
        const int length = shortLength - ecc + (i < shortBlocks ? 0 : 1);
        Bytes block(from, from + length);
        from += length;
        const Bytes check = remainder(block, by);
        if (i < shortBlocks)
            block.push_back(0); // where a long block has one more; skipped below
        block.insert(block.end(), check.begin(), check.end());
        blocks.push_back(std::move(block));
    }
    Bytes result;
    for (std::size_t i = 0; i < blocks.front().size(); ++i) {
        for (std::size_t j = 0; j < blocks.size(); ++j) {
            if (std::cmp_not_equal(i, shortLength - ecc) || std::cmp_greater_equal(j, shortBlocks))
                result.push_back(blocks.at(j).at(i));
        }
    }
    return result;
}

// The modules as they are drawn, and which of them are patterns rather than data.
struct Grid {
    int size;
    std::vector<bool> dark;
    std::vector<bool> fixed;

    explicit Grid(int side)
        : size(side), dark(static_cast<std::size_t>(side * side), false), fixed(static_cast<std::size_t>(side * side), false) {}
    [[nodiscard]] std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
    }
    [[nodiscard]] bool at(int x, int y) const { return dark.at(index(x, y)); }
    void pattern(int x, int y, bool isDark) {
        dark.at(index(x, y)) = isDark;
        fixed.at(index(x, y)) = true;
    }
};

void finder(Grid &grid, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int x = cx + dx;
            const int y = cy + dy;
            const int ring = std::max(std::abs(dx), std::abs(dy));
            if (x >= 0 && x < grid.size && y >= 0 && y < grid.size)
                grid.pattern(x, y, ring != 2 && ring != 4);
        }
    }
}

void alignment(Grid &grid, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            grid.pattern(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

// The format information, twice: level M and the mask, with its BCH bits.
void format(Grid &grid, int mask) {
    const unsigned data = levelBits << 3U | static_cast<unsigned>(mask);
    unsigned rest = data;
    for (int i = 0; i < 10; ++i)
        rest = (rest << 1U) ^ ((rest >> 9U) * 0x537U);
    const unsigned bits = (data << 10U | rest) ^ 0x5412U;
    const auto bit = [bits](int i) { return ((bits >> static_cast<unsigned>(i)) & 1U) != 0; };
    const int last = grid.size - 1;
    for (int i = 0; i <= 5; ++i)
        grid.pattern(8, i, bit(i));
    grid.pattern(8, 7, bit(6));
    grid.pattern(8, 8, bit(7));
    grid.pattern(7, 8, bit(8));
    for (int i = 9; i < 15; ++i)
        grid.pattern(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i)
        grid.pattern(last - i, 8, bit(i));
    for (int i = 8; i < 15; ++i)
        grid.pattern(8, grid.size - 15 + i, bit(i));
    grid.pattern(8, grid.size - 8, true); // the dark module, always there
}

// The version information, from version 7 on: twice, beside two of the finders.
void versionBits(Grid &grid, int version) {
    if (version < 7)
        return;
    auto rest = static_cast<unsigned>(version);
    for (int i = 0; i < 12; ++i)
        rest = (rest << 1U) ^ ((rest >> 11U) * 0x1F25U);
    const unsigned bits = static_cast<unsigned>(version) << 12U | rest;
    for (int i = 0; i < 18; ++i) {
        const bool isDark = ((bits >> static_cast<unsigned>(i)) & 1U) != 0;
        const int a = grid.size - 11 + i % 3;
        const int b = i / 3;
        grid.pattern(a, b, isDark);
        grid.pattern(b, a, isDark);
    }
}

void patterns(Grid &grid, int version) {
    for (int i = 0; i < grid.size; ++i) {
        grid.pattern(6, i, i % 2 == 0);
        grid.pattern(i, 6, i % 2 == 0);
    }
    finder(grid, 3, 3);
    finder(grid, grid.size - 4, 3);
    finder(grid, 3, grid.size - 4);
    const std::vector<int> centres = alignments(version);
    const std::size_t n = centres.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const bool underFinder = (i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0);
            if (!underFinder)
                alignment(grid, centres.at(i), centres.at(j));
        }
    }
    format(grid, 0); // reserved now, drawn for the mask chosen later
    versionBits(grid, version);
}

// The codewords in the standard's zigzag: two columns at a time from the
// right, up then down, around the patterns.
void place(Grid &grid, const Bytes &codewords) {
    std::size_t i = 0;
    const std::size_t total = codewords.size() * 8;
    for (int right = grid.size - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5; // past the vertical timing pattern
        const bool upward = (static_cast<unsigned>(right + 1) & 2U) == 0;
        for (int step = 0; step < grid.size; ++step) {
            const int y = upward ? grid.size - 1 - step : step;
            for (int x = right; x >= right - 1; --x) {
                if (grid.fixed.at(grid.index(x, y)) || i >= total)
                    continue;
                grid.dark.at(grid.index(x, y)) = ((static_cast<unsigned>(codewords.at(i / 8)) >> (7 - i % 8)) & 1U) != 0;
                ++i;
            }
        }
    }
}

[[nodiscard]] bool masked(int mask, int x, int y) {
    switch (mask) {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return x * y % 2 + x * y % 3 == 0;
    case 6:
        return (x * y % 2 + x * y % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

void applyMask(Grid &grid, int mask) {
    for (int y = 0; y < grid.size; ++y) {
        for (int x = 0; x < grid.size; ++x) {
            if (!grid.fixed.at(grid.index(x, y)) && masked(mask, x, y))
                grid.dark.at(grid.index(x, y)) = !grid.dark.at(grid.index(x, y));
        }
    }
}

// The runs of a line, for the penalty of what looks like a finder: the last
// seven lengths, newest first, with the light margin counted at either end.
struct Runs {
    int size = 0;
    std::array<int, 7> history{};

    void add(int length) {
        if (history.front() == 0)
            length += size; // the light margin before the line
        std::ranges::rotate(history, std::prev(history.end()));
        history.front() = length;
    }
    [[nodiscard]] int finders() const {
        const int n = history.at(1);
        const bool core = n > 0 && history.at(2) == n && history.at(3) == n * 3 && history.at(4) == n && history.at(5) == n;
        return (core && history.at(0) >= n * 4 && history.at(6) >= n ? 1 : 0)
               + (core && history.at(6) >= n * 4 && history.at(0) >= n ? 1 : 0);
    }
    [[nodiscard]] int end(bool dark, int length) {
        if (dark) {
            add(length);
            length = 0;
        }
        add(length + size); // the light margin after the line
        return finders();
    }
};

// The penalty of one line, a row or a column: runs of five or more alike, and finder-like patterns.
[[nodiscard]] long linePenalty(const Grid &grid, int line, bool row) {
    long result = 0;
    Runs runs{grid.size};
    bool colour = false;
    int length = 0;
    for (int i = 0; i < grid.size; ++i) {
        const bool here = row ? grid.at(i, line) : grid.at(line, i);
        if (here == colour) {
            ++length;
            if (length == 5)
                result += 3;
            else if (length > 5)
                ++result;
            continue;
        }
        runs.add(length);
        if (!colour)
            result += runs.finders() * 40L;
        colour = here;
        length = 1;
    }
    return result + runs.end(colour, length) * 40L;
}

[[nodiscard]] long penalty(const Grid &grid) {
    long result = 0;
    for (int i = 0; i < grid.size; ++i)
        result += linePenalty(grid, i, true) + linePenalty(grid, i, false);
    for (int y = 0; y + 1 < grid.size; ++y) {
        for (int x = 0; x + 1 < grid.size; ++x) {
            const bool colour = grid.at(x, y);
            if (colour == grid.at(x + 1, y) && colour == grid.at(x, y + 1) && colour == grid.at(x + 1, y + 1))
                result += 3;
        }
    }
    const auto darkCount = static_cast<long>(std::ranges::count(grid.dark, true));
    const long total = static_cast<long>(grid.size) * grid.size;
    const long k = (std::abs(darkCount * 20 - total * 10) + total - 1) / total - 1;
    return result + k * 10;
}

} // namespace

QrCode::QrCode(int version, int mask, std::vector<bool> modules)
    : m_version(version), m_size(sizeOf(version)), m_mask(mask), m_modules(std::move(modules)) {}

bool QrCode::dark(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_size || y >= m_size)
        return false;
    return m_modules.at(static_cast<std::size_t>(y) * static_cast<std::size_t>(m_size) + static_cast<std::size_t>(x));
}

std::optional<QrCode> QrCode::of(const QByteArray &bytes) {
    int version = 1;
    // The mode (4 bits), the count and the bytes.
    const qsizetype bits = bytes.size() * qsizetype{8};
    while (version <= largestVersion && 4 + (version < 10 ? 8 : 16) + bits > qsizetype{dataCodewords(version)} * 8)
        ++version;
    if (version > largestVersion)
        return std::nullopt;
    Grid grid(sizeOf(version));
    patterns(grid, version);
    place(grid, interleaved(encoded(bytes, version), version));
    // The mask whose penalty is least, the first of those on a tie.
    int best = 0;
    long least = -1;
    for (int mask = 0; mask < 8; ++mask) {
        Grid tried = grid;
        applyMask(tried, mask);
        format(tried, mask);
        if (const long score = penalty(tried); least < 0 || score < least) {
            least = score;
            best = mask;
        }
    }
    applyMask(grid, best);
    format(grid, best);
    return QrCode(version, best, std::move(grid.dark));
}
