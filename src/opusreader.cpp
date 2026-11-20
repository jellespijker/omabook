#include "opusreader.h"

#include <QtEndian>
#include <algorithm>
#include <array>
#include <iterator>
#include <opus.h>
#include <ranges>
#include <utility>
#include <vector>

#include "failure.h"

using namespace Qt::StringLiterals;

namespace Opus {

namespace {

// Decoded from this far before a place sought, the sound at the place is as
// it was (RFC 7845, 4.6).
constexpr std::int64_t preroll = 3840;
// The longest a packet decodes to: 120 ms.
constexpr int longestPacket = 5760;

// The bytes of a packet as the codec takes them, unsigned: a copy, of a
// packet of a hundred bytes or so, rather than a cast.
[[nodiscard]] std::vector<unsigned char> bytesOf(const QByteArray &packet) {
    return packet | std::views::transform([](char byte) { return static_cast<unsigned char>(byte); }) | std::ranges::to<std::vector>();
}

[[nodiscard]] QDateTime dateIn(const QByteArray &tags) {
    if (!tags.startsWith("OpusTags") || tags.size() < 16)
        return {};
    qsizetype at = 8;
    const auto word = [&tags](qsizetype from) {
        return static_cast<qsizetype>(qFromLittleEndian<std::uint32_t>(tags.mid(from, 4).constData()));
    };
    at += 4 + word(at);
    if (at + 4 > tags.size())
        return {};
    const qsizetype count = word(at);
    at += 4;
    for (qsizetype i = 0; i < count && at + 4 <= tags.size(); ++i) {
        const qsizetype size = word(at);
        const QByteArray comment = tags.mid(at + 4, size);
        at += 4 + size;
        if (comment.startsWith("DATE="))
            return QDateTime::fromString(QString::fromUtf8(comment.mid(5)), Qt::ISODateWithMs);
    }
    return {};
}

} // namespace

void Reader::Free::operator()(OpusDecoder *decoder) const { opus_decoder_destroy(decoder); }

Reader::~Reader() = default;

std::expected<std::unique_ptr<Reader>, QString> Reader::open(const QString &path) {
    auto reader = std::make_unique<Reader>(Key{});
    if (auto begun = reader->begin(path); !begun)
        return std::unexpected(begun.error());
    return reader;
}

std::expected<void, QString> Reader::begin(const QString &path) {
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::SoundFileUnreadable, {path, m_file.errorString()}).text());
    if (auto read = readHeaders(path); !read)
        return read;
    m_map = m_pages.map(m_pages.position());
    // Where the last page that ends a packet says the sound ends.
    for (const Ogg::Mark &mark : std::as_const(m_map))
        if (mark.granule >= 0)
            m_end = mark.granule;
    m_end = std::max<std::int64_t>(m_end, m_preskip);
    int error = OPUS_OK;
    m_decoder.reset(opus_decoder_create(sampleRate, 1, &error));
    if (error != OPUS_OK || !m_decoder)
        return std::unexpected(Failure(Failure::Code::OpusDecoderUnstarted, {QString::fromUtf8(opus_strerror(error))}).text());
    seek(0);
    return {};
}

std::expected<void, QString> Reader::readHeaders(const QString &path) {
    const std::optional<Ogg::Page> first = m_pages.next();
    const QByteArray head = first && !first->packets.isEmpty() ? first->packets.first() : QByteArray();
    if (!head.startsWith("OpusHead") || head.size() < 19 || head.at(9) < 1 || head.at(9) > 2)
        return std::unexpected(Failure(Failure::Code::NotOpus, {path}).text());
    m_preskip = qFromLittleEndian<std::uint16_t>(head.mid(10, 2).constData());
    // The tags may take more than a page; the sound begins after them.
    QByteArray tags;
    while (true) {
        const std::optional<Ogg::Page> page = m_pages.next();
        if (!page)
            return std::unexpected(Failure(Failure::Code::OpusWithoutSound, {path}).text());
        for (const QByteArray &packet : page->packets)
            tags += packet;
        if (!page->open)
            break;
    }
    m_started = dateIn(tags);
    return {};
}

void Reader::seek(std::int64_t sample) {
    sample = std::clamp<std::int64_t>(sample, 0, length());
    const std::int64_t from = m_preskip + sample;
    // The last page that ends far enough before the place: decoding starts
    // with the page after it.
    qsizetype before = -1;
    for (qsizetype i = 0; i < m_map.size(); ++i)
        if (m_map.at(i).granule >= 0 && m_map.at(i).granule <= from - preroll)
            before = i;
    opus_decoder_ctl(m_decoder.get(), OPUS_RESET_STATE);
    m_granule = before >= 0 ? m_map.at(before).granule : 0;
    m_skip = from;
    m_position = sample;
    m_pcm.clear();
    m_pcmAt = 0;
    m_carried.clear();
    const qsizetype start = before + 1;
    m_ended = start >= m_map.size() || sample >= length() || !m_pages.seek(m_map.at(start).offset);
}

qsizetype Reader::read(std::span<qint16> out) {
    qsizetype filled = 0;
    while (std::cmp_less(filled, out.size())) {
        if (m_pcmAt == m_pcm.size()) {
            m_pcm.clear();
            m_pcmAt = 0;
            if (!decodePage())
                break;
            continue;
        }
        const qsizetype taken = std::min(static_cast<qsizetype>(out.size()) - filled, m_pcm.size() - m_pcmAt);
        std::copy_n(m_pcm.constBegin() + m_pcmAt, taken, out.begin() + filled);
        m_pcmAt += taken;
        filled += taken;
    }
    m_position += filled;
    return filled;
}

bool Reader::decodePage() {
    if (m_ended)
        return false;
    const std::optional<Ogg::Page> page = m_pages.next();
    if (!page) {
        m_ended = true;
        return false;
    }
    fillGapBefore(*page);
    for (qsizetype i = 0; i < page->packets.size() && !m_ended; ++i) {
        QByteArray packet = page->packets.at(i);
        if (i == 0 && page->continued) {
            // The end of a packet whose start was not read (a seek landed
            // here) is no packet.
            if (m_carried.isEmpty())
                continue;
            packet = m_carried + packet;
            m_carried.clear();
        }
        if (i == page->packets.size() - 1 && page->open) {
            m_carried += packet;
            continue;
        }
        decode(packet);
    }
    m_ended = m_ended || page->last;
    return true;
}

// A page left out (its checksum was wrong) is silence, so what comes after
// it stays at its moment: a whole page says where it begins.
void Reader::fillGapBefore(const Ogg::Page &page) {
    if (page.continued || page.open || page.granule < 0)
        return;
    std::int64_t samples = 0;
    for (const QByteArray &packet : page.packets)
        samples += std::max(0, opus_packet_get_nb_samples(bytesOf(packet).data(), static_cast<opus_int32>(packet.size()), sampleRate));
    if (const std::int64_t gap = page.granule - samples - m_granule; gap > 0)
        place({}, gap);
}

void Reader::decode(const QByteArray &packet) {
    std::array<qint16, longestPacket> pcm{};
    int decoded =
        opus_decode(m_decoder.get(), bytesOf(packet).data(), static_cast<opus_int32>(packet.size()), pcm.data(), longestPacket, 0);
    // A packet that can't be decoded is silence as long as it was meant to
    // be, so what comes after stays at its moment.
    if (decoded < 0)
        decoded = std::clamp(opus_packet_get_nb_samples(bytesOf(packet).data(), static_cast<opus_int32>(packet.size()), sampleRate), 0,
                             longestPacket);
    place(std::span<const qint16>(pcm.data(), static_cast<std::size_t>(decoded)), decoded);
}

void Reader::place(std::span<const qint16> pcm, std::int64_t count) {
    const std::int64_t begins = m_granule;
    const std::int64_t ends = std::min(m_granule + count, m_end);
    m_granule += count;
    const std::int64_t from = std::max(begins, m_skip);
    if (from < ends) {
        if (pcm.empty())
            m_pcm.resize(m_pcm.size() + static_cast<qsizetype>(ends - from), 0);
        else
            std::ranges::copy(pcm.subspan(static_cast<std::size_t>(from - begins), static_cast<std::size_t>(ends - from)),
                              std::back_inserter(m_pcm));
    }
    m_ended = m_granule >= m_end;
}

} // namespace Opus
