#include "zipedit.h"

#include <QtEndian>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace {

using namespace Qt::StringLiterals;

constexpr quint32 localSignature = 0x04034b50;
constexpr quint32 centralSignature = 0x02014b50;
constexpr quint32 endSignature = 0x06054b50;
constexpr quint32 zip64LocatorSignature = 0x07064b50;
constexpr qsizetype localSize = 30;
constexpr qsizetype centralSize = 46;
constexpr qsizetype endSize = 22;
constexpr quint16 encrypted = 1U << 0;
constexpr quint16 utf8Names = 1U << 11;
constexpr quint16 deflated = 8;
constexpr quint16 versionNeeded = 20;
constexpr quint32 tooLarge = std::numeric_limits<quint32>::max();
// Made on Unix by zip 3.0, a file anyone may read: what a new record says.
constexpr quint16 madeBy = (3U << 8) | 30U;
constexpr quint32 fileAttributes = 0100644U << 16;
// 1980-01-01 at midnight, the first day a zip can say, for a file with no time of its own.
constexpr quint16 firstDay = (1U << 5) | 1U;

[[nodiscard]] quint16 u16(QByteArrayView data, qsizetype at) { return qFromLittleEndian<quint16>(data.data() + at); }
[[nodiscard]] quint32 u32(QByteArrayView data, qsizetype at) { return qFromLittleEndian<quint32>(data.data() + at); }

void put16(QByteArray &out, quint16 value) {
    std::array<char, 2> bytes{};
    qToLittleEndian(value, bytes.data());
    out.append(bytes.data(), bytes.size());
}

void put32(QByteArray &out, quint32 value) {
    std::array<char, 4> bytes{};
    qToLittleEndian(value, bytes.data());
    out.append(bytes.data(), bytes.size());
}

void set16(QByteArray &out, qsizetype at, quint16 value) { qToLittleEndian(value, out.data() + at); }
void set32(QByteArray &out, qsizetype at, quint32 value) { qToLittleEndian(value, out.data() + at); }

[[nodiscard]] std::unexpected<Failure> unchangeable() { return std::unexpected(Failure(Failure::Code::ZipUnchangeable)); }

// A file as the central directory says it, and where that says it.
struct Central {
    ZipEdit::Entry entry;
    qsizetype at = 0;   // where its central record begins
    qsizetype size = 0; // how long that record is
};

struct Directory {
    QList<Central> files; // in the order their records lie
    qsizetype start = 0;  // where the central directory begins
    QByteArrayView comment;
};

// Where the end of central directory record is: the last one whose comment
// runs to the end of the file.
[[nodiscard]] std::optional<qsizetype> endRecord(QByteArrayView zip) {
    const qsizetype lowest = std::max<qsizetype>(0, zip.size() - endSize - 0xFFFF);
    for (qsizetype at = zip.size() - endSize; at >= lowest; --at)
        if (u32(zip, at) == endSignature && at + endSize + u16(zip, at + 20) == zip.size())
            return at;
    return std::nullopt;
}

[[nodiscard]] std::optional<Central> centralAt(QByteArrayView zip, qsizetype at, qsizetype end) {
    if (at + centralSize > end || u32(zip, at) != centralSignature)
        return std::nullopt;
    const quint16 nameLength = u16(zip, at + 28);
    const qsizetype size = centralSize + nameLength + u16(zip, at + 30) + u16(zip, at + 32);
    // One on another disk, or a zip64 one, whose sizes and place are elsewhere.
    if (at + size > end || u16(zip, at + 34) != 0 || u32(zip, at + 20) == tooLarge || u32(zip, at + 24) == tooLarge
        || u32(zip, at + 42) == tooLarge)
        return std::nullopt;
    const quint16 flags = u16(zip, at + 8);
    const QByteArrayView name = zip.sliced(at + centralSize, nameLength);
    return Central{.entry = {.name = (flags & utf8Names) != 0 ? QString::fromUtf8(name) : QString::fromLatin1(name),
                             .method = u16(zip, at + 10),
                             .flags = flags,
                             .crc = u32(zip, at + 16),
                             .offset = u32(zip, at + 42),
                             .length = 0,
                             .localExtra = 0},
                   .at = at,
                   .size = size};
}

// Each record runs to where the next begins, and holds at least its header
// and its compressed bytes.
[[nodiscard]] bool measured(QByteArrayView zip, QList<Central> &files, qsizetype start) {
    std::ranges::sort(files, {}, [](const Central &file) { return file.entry.offset; });
    for (qsizetype i = 0; i < files.size(); ++i) {
        ZipEdit::Entry &entry = files[i].entry;
        const qsizetype next = i + 1 < files.size() ? files.at(i + 1).entry.offset : start;
        if (entry.offset + localSize > next || u32(zip, entry.offset) != localSignature)
            return false;
        entry.localExtra = u16(zip, entry.offset + 28);
        entry.length = next - entry.offset;
        const qsizetype data = localSize + u16(zip, entry.offset + 26) + entry.localExtra;
        if (data + u32(zip, files.at(i).at + 20) > entry.length)
            return false;
    }
    return true;
}

[[nodiscard]] std::expected<Directory, Failure> directory(QByteArrayView zip) {
    const std::optional<qsizetype> end = endRecord(zip);
    if (!end)
        return unchangeable();
    const quint16 count = u16(zip, *end + 10);
    const quint32 size = u32(zip, *end + 12);
    const quint32 start = u32(zip, *end + 16);
    // Spanning disks, or a zip64, which says its directory elsewhere.
    const bool zip64 =
        (*end >= 20 && u32(zip, *end - 20) == zip64LocatorSignature) || count == 0xFFFF || size == tooLarge || start == tooLarge;
    if (zip64 || u16(zip, *end + 4) != 0 || u16(zip, *end + 6) != 0 || u16(zip, *end + 8) != count
        || qsizetype{start} + qsizetype{size} > *end)
        return unchangeable();
    Directory found{.files = {}, .start = start, .comment = zip.sliced(*end + endSize)};
    for (qsizetype at = start, i = 0; i < count; ++i) {
        const std::optional<Central> file = centralAt(zip, at, start + qsizetype{size});
        if (!file)
            return unchangeable();
        found.files << *file;
        at += file->size;
    }
    if (!measured(zip, found.files, start))
        return unchangeable();
    return found;
}

// Raw deflate: what qCompress makes, without zlib's header and checksum.
[[nodiscard]] QByteArray deflate(const QByteArray &data) {
    const QByteArray zlib = qCompress(data, 9);
    constexpr qsizetype before = 4 + 2, after = 4; // its length, zlib's header; Adler-32
    return zlib.size() > before + after ? zlib.mid(before, zlib.size() - before - after) : QByteArray();
}

// A file written anew: in place of one (whose central record `central` is,
// with its name and times) or a new one.
struct Written {
    QByteArray name;
    QByteArray contents;
    bool store = false;
    QByteArrayView central; // empty for a new file
};

class Writer {
public:
    explicit Writer(qsizetype size) { m_out.reserve(size + 4096); }

    void copy(QByteArrayView zip, const Central &file) {
        QByteArray record = zip.sliced(file.at, file.size).toByteArray();
        set32(record, 42, static_cast<quint32>(m_out.size()));
        m_out.append(zip.sliced(file.entry.offset, file.entry.length));
        m_directory += record;
        ++m_count;
    }

    void write(const Written &file) {
        const QByteArray compressed = file.store || file.contents.isEmpty() ? QByteArray() : deflate(file.contents);
        const bool store = compressed.isEmpty() || compressed.size() >= file.contents.size();
        const QByteArray &data = store ? file.contents : compressed;
        const quint16 method = store ? 0 : deflated;
        const quint32 crc = ZipEdit::crc32(file.contents);
        const bool replacing = !file.central.isEmpty();
        // Of its bits only how its name is written stays: its sizes come
        // before it now, and it isn't encrypted.
        const auto flags = static_cast<quint16>(replacing ? u16(file.central, 8) & utf8Names : utf8Names);
        const quint16 time = replacing ? u16(file.central, 12) : 0;
        const quint16 date = replacing ? u16(file.central, 14) : firstDay;
        const auto offset = static_cast<quint32>(m_out.size());
        put32(m_out, localSignature);
        for (const quint16 field : {versionNeeded, flags, method, time, date})
            put16(m_out, field);
        for (const quint32 field : {crc, static_cast<quint32>(data.size()), static_cast<quint32>(file.contents.size())})
            put32(m_out, field);
        put16(m_out, static_cast<quint16>(file.name.size()));
        put16(m_out, 0);
        m_out += file.name;
        m_out += data;
        m_directory += replacing ? patched(file.central, {flags, method, crc, data.size(), file.contents.size(), offset})
                                 : fresh(file.name, {flags, method, crc, data.size(), file.contents.size(), offset});
        ++m_count;
    }

    [[nodiscard]] std::expected<QByteArray, Failure> finish(QByteArrayView comment) {
        if (m_count > 0xFFFE || m_out.size() + m_directory.size() >= qsizetype{tooLarge})
            return unchangeable();
        const auto start = static_cast<quint32>(m_out.size());
        m_out += m_directory;
        put32(m_out, endSignature);
        for (const quint16 field : {quint16{0}, quint16{0}, static_cast<quint16>(m_count), static_cast<quint16>(m_count)})
            put16(m_out, field);
        put32(m_out, static_cast<quint32>(m_directory.size()));
        put32(m_out, start);
        put16(m_out, static_cast<quint16>(comment.size()));
        m_out.append(comment);
        return m_out;
    }

private:
    struct Said {
        quint16 flags, method;
        quint32 crc;
        qsizetype compressed, size;
        quint32 offset;
    };

    [[nodiscard]] static QByteArray patched(QByteArrayView central, const Said &said) {
        QByteArray record = central.toByteArray();
        set16(record, 6, versionNeeded);
        set16(record, 8, said.flags);
        set16(record, 10, said.method);
        set32(record, 16, said.crc);
        set32(record, 20, static_cast<quint32>(said.compressed));
        set32(record, 24, static_cast<quint32>(said.size));
        set32(record, 42, said.offset);
        return record;
    }

    [[nodiscard]] static QByteArray fresh(const QByteArray &name, const Said &said) {
        QByteArray record;
        put32(record, centralSignature);
        for (const quint16 field : {madeBy, versionNeeded, said.flags, said.method, quint16{0}, firstDay})
            put16(record, field);
        for (const quint32 field : {said.crc, static_cast<quint32>(said.compressed), static_cast<quint32>(said.size)})
            put32(record, field);
        for (const quint16 field : {static_cast<quint16>(name.size()), quint16{0}, quint16{0}, quint16{0}, quint16{0}})
            put16(record, field);
        put32(record, fileAttributes);
        put32(record, said.offset);
        return record + name;
    }

    QByteArray m_out, m_directory;
    qsizetype m_count = 0;
};

// Whether the first record is the mimetype EPUB asks for, to copy as it is.
[[nodiscard]] bool rightMimetype(QByteArrayView zip, const Directory &found) {
    if (found.files.isEmpty())
        return false;
    const Central &first = found.files.first();
    const qsizetype data = first.entry.offset + localSize + u16(zip, first.entry.offset + 26) + first.entry.localExtra;
    return first.entry.name == "mimetype"_L1 && first.entry.method == 0 && first.entry.localExtra == 0
           && zip.sliced(data, u32(zip, first.at + 20)) == "application/epub+zip"_ba;
}

// Whether every file to replace is there and can be read, and none to add is.
[[nodiscard]] bool fits(const Directory &found, const ZipEdit::Change &change) {
    const auto readable = [&found](const QString &name) {
        return std::ranges::any_of(found.files,
                                   [&name](const Central &file) { return file.entry.name == name && (file.entry.flags & encrypted) == 0; });
    };
    const auto there = [&found](const QString &name) {
        return std::ranges::any_of(found.files, [&name](const Central &file) { return file.entry.name == name; });
    };
    return std::ranges::all_of(change.replaced.keys(), readable)
           && std::ranges::none_of(change.added, [&there](const auto &file) { return there(file.first); });
}

} // namespace

quint32 ZipEdit::crc32(QByteArrayView data) noexcept {
    static constexpr auto table = [] {
        std::array<quint32, 256> made{};
        for (quint32 n = 0; n < made.size(); ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            made.at(n) = c;
        }
        return made;
    }();
    quint32 crc = 0xFFFFFFFFU;
    for (const char byte : data)
        crc = table.at((crc ^ static_cast<quint8>(byte)) & 0xFFU) ^ (crc >> 8U);
    return crc ^ 0xFFFFFFFFU;
}

std::expected<QList<ZipEdit::Entry>, Failure> ZipEdit::entries(QByteArrayView zip) {
    return directory(zip).transform([](const Directory &found) {
        QList<Entry> files;
        for (const Central &file : found.files)
            files << file.entry;
        return files;
    });
}

std::expected<QByteArray, Failure> ZipEdit::edited(QByteArrayView zip, const Change &change) {
    const auto found = directory(zip);
    if (!found)
        return std::unexpected(found.error());
    if (!fits(*found, change))
        return unchangeable();
    Writer writer(zip.size());
    const bool keepMimetype = change.epub && rightMimetype(zip, *found);
    if (change.epub && !keepMimetype)
        writer.write({.name = "mimetype"_ba, .contents = "application/epub+zip"_ba, .store = true, .central = {}});
    for (const Central &file : found->files) {
        if (change.epub && !keepMimetype && file.entry.name == "mimetype"_L1)
            continue;
        if (const auto replaced = change.replaced.constFind(file.entry.name); replaced != change.replaced.cend())
            writer.write({.name = zip.sliced(file.at + centralSize, u16(zip, file.at + 28)).toByteArray(),
                          .contents = *replaced,
                          .store = false,
                          .central = zip.sliced(file.at, file.size)});
        else
            writer.copy(zip, file);
    }
    for (const auto &[name, contents] : change.added)
        writer.write({.name = name.toUtf8(), .contents = contents, .store = false, .central = {}});
    return writer.finish(found->comment);
}
