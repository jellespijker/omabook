#include "oggpages.h"

#include <QtEndian>
#include <array>
#include <tuple>

#include "failure.h"

using namespace Qt::StringLiterals;

namespace Ogg {

namespace {

constexpr int headerSize = 27;
constexpr int maxSegments = 255;
constexpr unsigned continuedFlag = 0x01U;
constexpr unsigned firstFlag = 0x02U;
constexpr unsigned lastFlag = 0x04U;

constexpr std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> crcs{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i << 24U;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04c11db7U : crc << 1U;
        crcs.at(i) = crc;
    }
    return crcs;
}();

template <typename T> void put(QByteArray &bytes, qsizetype at, T value) {
    std::array<char, sizeof(T)> little{};
    qToLittleEndian<T>(value, little.data());
    bytes.replace(at, little.size(), QByteArrayView(little.data(), little.size()));
}

template <typename T> [[nodiscard]] T take(const QByteArray &bytes, qsizetype at) {
    return qFromLittleEndian<T>(bytes.mid(at, sizeof(T)).constData());
}

} // namespace

std::uint32_t checksum(QByteArrayView page) {
    std::uint32_t crc = 0;
    for (const char byte : page)
        crc = (crc << 8U) ^ table.at(((crc >> 24U) ^ static_cast<std::uint8_t>(byte)) & 0xffU);
    return crc;
}

std::expected<void, QString> Writer::add(QByteArrayView packet, std::int64_t granule) {
    const qsizetype segments = packet.size() / 255 + 1;
    if (segments > maxSegments)
        return std::unexpected(Failure(Failure::Code::PacketTooLarge, {QString::number(packet.size())}).text());
    if (m_lacing.size() + segments > maxSegments)
        if (auto written = flush(); !written)
            return written;
    m_lacing.append(QByteArray(segments - 1, '\xff'));
    m_lacing.append(static_cast<char>(packet.size() % 255));
    m_body.append(packet);
    m_granule = granule;
    ++m_packets;
    return {};
}

std::expected<void, QString> Writer::flush(bool last) {
    if (m_packets == 0 && !last)
        return {};
    QByteArray page(headerSize, '\0');
    page.replace(0, 4, "OggS");
    const auto flags = static_cast<std::uint8_t>((m_sequence == 0 ? firstFlag : 0U) | (last ? lastFlag : 0U));
    put<std::uint8_t>(page, 5, flags);
    put<std::int64_t>(page, 6, m_packets == 0 ? -1 : m_granule);
    put<std::uint32_t>(page, 14, m_serial);
    put<std::uint32_t>(page, 18, m_sequence++);
    put<std::uint8_t>(page, 26, static_cast<std::uint8_t>(m_lacing.size()));
    page += m_lacing + m_body;
    put<std::uint32_t>(page, 22, checksum(page));
    m_lacing.clear();
    m_body.clear();
    m_packets = 0;
    if (m_device->write(page) != page.size())
        return std::unexpected(Failure(Failure::Code::RecordingUnwritten, {m_device->errorString()}).text());
    return {};
}

std::optional<Reader::Header> Reader::header() {
    Header found{.bytes = m_device->read(headerSize), .body = 0};
    if (found.bytes.size() < headerSize || !found.bytes.startsWith("OggS") || found.bytes.at(4) != 0)
        return std::nullopt;
    const int segments = take<std::uint8_t>(found.bytes, 26);
    const QByteArray lacing = m_device->read(segments);
    if (lacing.size() < segments)
        return std::nullopt;
    for (const char value : lacing)
        found.body += static_cast<std::uint8_t>(value);
    found.bytes += lacing;
    return found;
}

std::optional<Page> Reader::next() {
    while (true) {
        const std::optional<Header> found = header();
        if (!found)
            return std::nullopt;
        const QByteArray body = m_device->read(found->body);
        if (body.size() < found->body)
            return std::nullopt;
        QByteArray whole = found->bytes + body;
        const auto crc = take<std::uint32_t>(whole, 22);
        put<std::uint32_t>(whole, 22, 0);
        if (checksum(whole) != crc)
            continue;
        const auto flags = static_cast<unsigned>(take<std::uint8_t>(whole, 5));
        Page page{.granule = take<std::int64_t>(whole, 6),
                  .first = (flags & firstFlag) != 0,
                  .last = (flags & lastFlag) != 0,
                  .continued = (flags & continuedFlag) != 0,
                  .packets = {},
                  .open = false};
        qsizetype at = 0;
        qsizetype length = 0;
        bool ended = true;
        for (const char value : found->bytes.mid(headerSize)) {
            const auto size = static_cast<std::uint8_t>(value);
            length += size;
            ended = size < 255;
            if (ended) {
                page.packets << body.mid(at, length);
                at += length;
                length = 0;
            }
        }
        if (!ended) {
            page.packets << body.mid(at, length);
            page.open = true;
        }
        return page;
    }
}

QList<Mark> Reader::map(qint64 from) {
    const qint64 was = m_device->pos();
    QList<Mark> marks;
    qint64 offset = from;
    while (m_device->seek(offset)) {
        const std::optional<Header> found = header();
        if (!found || offset + found->bytes.size() + found->body > m_device->size())
            break;
        marks.append({.offset = offset, .granule = take<std::int64_t>(found->bytes, 6)});
        offset += found->bytes.size() + found->body;
    }
    std::ignore = m_device->seek(was); // back to where it was, which was readable
    return marks;
}

} // namespace Ogg
