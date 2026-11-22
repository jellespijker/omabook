#pragma once
// Reading a QR code back, as a phone's camera would once it has found it
// (#177): from a picture of it on a light margin, its modules sampled at
// their centres, its format information checked, the mask taken off, the
// codewords read in their zigzag, every block checked by its syndromes, and
// the bytes of its one byte-mode segment. Written apart from src/qrcode.cpp,
// from the standard, so that the one checks the other: level M and versions
// 1 to 10, what Omabook makes.

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace QrRead {

using Modules = std::vector<std::vector<bool>>; // [row][column]

// The modules of the code in a picture: dark is under half brightness.
inline std::optional<Modules> modulesIn(const QImage &picture) {
    // What isn't drawn on (a grabbed item's corners) is clear, not dark.
    const QImage image = picture.convertToFormat(QImage::Format_ARGB32);
    const auto darkAt = [&image](int x, int y) {
        const QRgb pixel = image.pixel(x, y);
        return qAlpha(pixel) >= 128 && qGray(pixel) < 128;
    };
    QRect box;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (darkAt(x, y))
                box |= QRect(x, y, 1, 1);
    if (box.isEmpty())
        return std::nullopt;
    // The top left finder's top edge is seven modules of dark.
    int run = 0;
    while (box.left() + run <= box.right() && darkAt(box.left() + run, box.top()))
        ++run;
    const double module = run / 7.0;
    const int size = static_cast<int>(std::lround(box.width() / module));
    if (size < 21 || (size - 17) % 4 != 0)
        return std::nullopt;
    Modules modules(static_cast<std::size_t>(size), std::vector<bool>(static_cast<std::size_t>(size)));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            modules[y][x] = darkAt(box.left() + static_cast<int>((x + 0.5) * module), box.top() + static_cast<int>((y + 0.5) * module));
    return modules;
}

// Whether a module is under an alignment pattern of a version.
inline bool aligning(int x, int y, int version) {
    if (version == 1)
        return false;
    const int size = 17 + 4 * version;
    const int count = version / 7 + 2;
    const int step = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
    std::vector<int> centres{6};
    for (int i = 1; i < count; ++i)
        centres.push_back(size - 7 - (count - 1 - i) * step);
    for (const int cy : centres)
        for (const int cx : centres) {
            const bool underFinder = (cx == 6 && cy == 6) || (cx == 6 && cy == size - 7) || (cx == size - 7 && cy == 6);
            if (!underFinder && std::abs(x - cx) <= 2 && std::abs(y - cy) <= 2)
                return true;
        }
    return false;
}

// Whether a module is part of a pattern rather than data, for a version.
inline bool reserved(int x, int y, int version) {
    const int size = 17 + 4 * version;
    const bool nearTop = y < 9, nearLeft = x < 9, nearRight = x >= size - 8, nearBottom = y >= size - 8;
    const bool finders = (nearTop && (nearLeft || nearRight)) || (nearBottom && nearLeft); // and the format information
    const bool versionInformation = version >= 7 && ((x >= size - 11 && y < 6) || (y >= size - 11 && x < 6));
    return finders || x == 6 || y == 6 || versionInformation || aligning(x, y, version);
}

inline std::uint8_t times(std::uint8_t a, std::uint8_t b) {
    std::uint8_t product = 0;
    while (b) {
        if (b & 1)
            product ^= a;
        a = static_cast<std::uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1D : 0));
        b >>= 1;
    }
    return product;
}

// Whether a block, its data and then its error correction, is a codeword: every syndrome zero.
inline bool sound(const std::vector<std::uint8_t> &block, int ecc) {
    std::uint8_t alpha = 1;
    for (int i = 0; i < ecc; ++i) {
        std::uint8_t syndrome = 0;
        for (const std::uint8_t c : block)
            syndrome = static_cast<std::uint8_t>(times(syndrome, alpha) ^ c);
        if (syndrome != 0)
            return false;
        alpha = times(alpha, 2);
    }
    return true;
}

// The data and error correction bits in their zigzag, the mask taken off:
// what two encoders that chose different masks agree on. None when the
// format information isn't level M's, or doesn't check.
inline std::optional<std::vector<bool>> dataBits(const Modules &modules) {
    const int size = static_cast<int>(modules.size());
    const int version = (size - 17) / 4;
    if (version < 1 || version > 10)
        return std::nullopt;
    const auto at = [&modules](int x, int y) { return static_cast<bool>(modules[y][x]); };
    // The format information beside the top left finder, bit 14 first.
    const std::array<std::pair<int, int>, 15> formatAt{
        {{0, 8}, {1, 8}, {2, 8}, {3, 8}, {4, 8}, {5, 8}, {7, 8}, {8, 8}, {8, 7}, {8, 5}, {8, 4}, {8, 3}, {8, 2}, {8, 1}, {8, 0}}};
    unsigned format = 0;
    for (const auto &[x, y] : formatAt)
        format = (format << 1) | (at(x, y) ? 1U : 0U);
    format ^= 0x5412;
    unsigned check = format >> 10;
    for (int i = 0; i < 10; ++i)
        check = (check << 1) ^ ((check >> 9) * 0x537);
    if (((format >> 10) << 10 | check) != format || (format >> 13) != 0) // level M is 00
        return std::nullopt;
    const int mask = static_cast<int>((format >> 10) & 7);
    const auto flipped = [mask](int x, int y) {
        const std::array<bool, 8> rules{(x + y) % 2 == 0,
                                        y % 2 == 0,
                                        x % 3 == 0,
                                        (x + y) % 3 == 0,
                                        (x / 3 + y / 2) % 2 == 0,
                                        (x * y) % 2 + (x * y) % 3 == 0,
                                        ((x * y) % 2 + (x * y) % 3) % 2 == 0,
                                        ((x + y) % 2 + (x * y) % 3) % 2 == 0};
        return rules[static_cast<std::size_t>(mask)];
    };
    // The bits in their zigzag, unmasked.
    std::vector<bool> bits;
    int column = size - 1;
    bool up = true;
    while (column > 0) {
        if (column == 6)
            --column;
        for (int step = 0; step < size; ++step) {
            const int y = up ? size - 1 - step : step;
            for (const int x : {column, column - 1})
                if (!reserved(x, y, version))
                    bits.push_back(at(x, y) != flipped(x, y));
        }
        up = !up;
        column -= 2;
    }
    return bits;
}

// The data codewords of every block in turn, each block checked; none when one doesn't check.
inline std::optional<QByteArray> blocksOf(const std::vector<std::uint8_t> &codewords, int version) {
    // Level M's blocks: the error correction of each, and how many.
    const std::array<int, 10> eccOf{10, 16, 26, 18, 24, 16, 18, 22, 22, 26}, countOf{1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
    const int ecc = eccOf[version - 1], blocks = countOf[version - 1];
    const int total = static_cast<int>(codewords.size());
    const int longBlocks = total % blocks, shortData = total / blocks - ecc;
    std::vector<std::vector<std::uint8_t>> data(static_cast<std::size_t>(blocks)), corrections(static_cast<std::size_t>(blocks));
    std::size_t next = 0;
    for (int i = 0; i < shortData + 1; ++i)
        for (int b = 0; b < blocks; ++b)
            if (i < shortData || b >= blocks - longBlocks)
                data[b].push_back(codewords.at(next++));
    for (int i = 0; i < ecc; ++i)
        for (int b = 0; b < blocks; ++b)
            corrections[b].push_back(codewords.at(next++));
    QByteArray stream;
    for (int b = 0; b < blocks; ++b) {
        std::vector<std::uint8_t> whole = data[b];
        whole.insert(whole.end(), corrections[b].begin(), corrections[b].end());
        if (!sound(whole, ecc))
            return std::nullopt;
        for (const std::uint8_t byte : data[b])
            stream.append(static_cast<char>(byte));
    }
    return stream;
}

// The bytes the code holds; none when it isn't one Omabook would make, or doesn't check.
inline std::optional<QByteArray> decode(const Modules &modules) {
    const auto unmasked = dataBits(modules);
    if (!unmasked)
        return std::nullopt;
    const std::vector<bool> &bits = *unmasked;
    const int version = (static_cast<int>(modules.size()) - 17) / 4;
    std::vector<std::uint8_t> codewords(bits.size() / 8);
    for (std::size_t i = 0; i < codewords.size() * 8; ++i)
        codewords[i / 8] = static_cast<std::uint8_t>(codewords[i / 8] << 1 | (bits[i] ? 1 : 0));
    const auto checked = blocksOf(codewords, version);
    if (!checked)
        return std::nullopt;
    const QByteArray &stream = *checked;
    // One segment in byte mode: 0100, the count, the bytes.
    std::size_t bit = 0;
    const auto read = [&stream, &bit](int length) {
        unsigned value = 0;
        for (int i = 0; i < length; ++i, ++bit)
            value = value << 1 | ((static_cast<std::uint8_t>(stream.at(static_cast<qsizetype>(bit / 8))) >> (7 - bit % 8)) & 1U);
        return value;
    };
    if (read(4) != 0b0100)
        return std::nullopt;
    const unsigned count = read(version < 10 ? 8 : 16);
    if ((bit + count * 8) > static_cast<std::size_t>(stream.size()) * 8)
        return std::nullopt;
    QByteArray bytes;
    for (unsigned i = 0; i < count; ++i)
        bytes.append(static_cast<char>(read(8)));
    return bytes;
}

// The bytes of the code in a picture.
inline std::optional<QByteArray> read(const QImage &picture) {
    const auto modules = modulesIn(picture);
    return modules ? decode(*modules) : std::nullopt;
}

} // namespace QrRead
