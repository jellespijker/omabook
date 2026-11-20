#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QIODevice>
#include <QList>
#include <QString>
#include <cstdint>
#include <expected>
#include <optional>

// Ogg, the container an Opus recording is kept in (RFC 3533): packets in
// pages, each page with its own checksum and the moment its last packet ends
// (the granule, in samples from the stream's start). A recording is written
// a page at a time, so what was written stays readable however the writing
// ends, and read a page at a time, so an hour of it never sits in memory.
namespace Ogg {

// The checksum of a page, as the format computes it (with its own field 0).
[[nodiscard]] std::uint32_t checksum(QByteArrayView page);

// Writes the pages of one stream, a packet at a time. A packet never runs
// on into the next page: the page is written first when it has no room.
class Writer {
public:
    Writer(QIODevice &device, std::uint32_t serial) : m_device(&device), m_serial(serial) {}

    // Adds a packet that ends `granule` samples into the stream to the page
    // being made.
    [[nodiscard]] std::expected<void, QString> add(QByteArrayView packet, std::int64_t granule);
    // Writes the page being made; `last` ends the stream with it (written
    // even when it holds no packet, so that the end is said).
    [[nodiscard]] std::expected<void, QString> flush(bool last = false);
    // The packets on the page being made.
    [[nodiscard]] int waiting() const noexcept { return m_packets; }

private:
    QIODevice *m_device; // never null
    std::uint32_t m_serial;
    std::uint32_t m_sequence = 0;
    QByteArray m_lacing, m_body;
    std::int64_t m_granule = -1;
    int m_packets = 0;
};

struct Page {
    std::int64_t granule = -1; // where its last finished packet ends; -1 when none does
    bool first = false, last = false;
    // Its first packet is the end of one the page before began.
    bool continued = false;
    // Its packets; the last of them goes on in the next page when `open`.
    QList<QByteArray> packets;
    bool open = false;
};

// Where a page starts in the file and the granule it ends at.
struct Mark {
    qint64 offset = 0;
    std::int64_t granule = -1;
};

// Reads the pages of a stream from a device that can seek.
class Reader {
public:
    explicit Reader(QIODevice &device) : m_device(&device) {}

    // The page at the device's position, and the position moved past it.
    // Nothing at the end of the file, or at a page cut short (a recording
    // whose writing was stopped half way); a page whose checksum is wrong
    // is skipped, the ones after it read.
    [[nodiscard]] std::optional<Page> next();
    [[nodiscard]] bool seek(qint64 offset) { return m_device->seek(offset); }
    [[nodiscard]] qint64 position() const { return m_device->pos(); }
    // Every whole page from `from` on, by its header alone: the map a seek
    // looks in. The position is left where it was.
    [[nodiscard]] QList<Mark> map(qint64 from);

private:
    struct Header {
        QByteArray bytes; // the header with its lacing
        qint64 body = 0;
    };
    [[nodiscard]] std::optional<Header> header();

    QIODevice *m_device; // never null
};

} // namespace Ogg
