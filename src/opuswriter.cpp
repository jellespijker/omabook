#include "opuswriter.h"

#include <QDir>
#include <QFileInfo>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <iterator>
#include <opus.h>
#include <tuple>

#include "failure.h"

using namespace Qt::StringLiterals;

namespace Opus {

namespace {

// A page a second: what a recording cut short loses at most.
constexpr int framesAPage = sampleRate / frameSamples;
constexpr int maxPacket = 1275;

template <typename T> void append(QByteArray &bytes, T value) {
    std::array<char, sizeof(T)> little{};
    qToLittleEndian<T>(value, little.data());
    bytes.append(little.data(), little.size());
}

// OpusHead: one channel, how many samples to leave out at the start, and the
// rate the sound was taken at.
[[nodiscard]] QByteArray head(int preskip) {
    QByteArray bytes("OpusHead");
    append<std::uint8_t>(bytes, 1);
    append<std::uint8_t>(bytes, 1);
    append<std::uint16_t>(bytes, static_cast<std::uint16_t>(preskip));
    append<std::uint32_t>(bytes, sampleRate);
    append<std::int16_t>(bytes, 0);
    append<std::uint8_t>(bytes, 0);
    return bytes;
}

// OpusTags: who wrote it, and when it was started (DATE, to the millisecond).
[[nodiscard]] QByteArray tags(const QDateTime &started) {
    const QByteArray vendor = "Omabook " OMABOOK_VERSION;
    const QByteArray date = "DATE=" + started.toUTC().toString(Qt::ISODateWithMs).toUtf8();
    QByteArray bytes("OpusTags");
    append<std::uint32_t>(bytes, static_cast<std::uint32_t>(vendor.size()));
    bytes += vendor;
    append<std::uint32_t>(bytes, 1);
    append<std::uint32_t>(bytes, static_cast<std::uint32_t>(date.size()));
    bytes += date;
    return bytes;
}

} // namespace

void Writer::Free::operator()(OpusEncoder *encoder) const { opus_encoder_destroy(encoder); }

std::expected<std::unique_ptr<Writer>, QString> Writer::open(const Options &options) {
    auto writer = std::make_unique<Writer>(Key{});
    if (auto begun = writer->begin(options); !begun)
        return std::unexpected(begun.error());
    return writer;
}

std::expected<void, QString> Writer::begin(const Options &options) {
    int error = OPUS_OK;
    m_encoder.reset(opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_VOIP, &error));
    if (error != OPUS_OK || !m_encoder)
        return std::unexpected(Failure(Failure::Code::OpusEncoderUnstarted, {QString::fromUtf8(opus_strerror(error))}).text());
    opus_encoder_ctl(m_encoder.get(), OPUS_SET_BITRATE(options.bitrate));
    opus_encoder_ctl(m_encoder.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(m_encoder.get(), OPUS_GET_LOOKAHEAD(&m_preskip));
    if (!QDir().mkpath(QFileInfo(options.path).absolutePath()))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {options.path}).text());
    m_file.setFileName(options.path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return std::unexpected(Failure(Failure::Code::SoundFileUnwritten, {options.path, m_file.errorString()}).text());
    // The two headers each on a page of their own, as the format asks.
    if (auto written = m_ogg.add(head(m_preskip), 0); !written)
        return written;
    if (auto written = m_ogg.flush(); !written)
        return written;
    if (auto written = m_ogg.add(tags(options.started), 0); !written)
        return written;
    if (auto written = m_ogg.flush(); !written)
        return written;
    m_file.flush();
    return {};
}

Writer::~Writer() {
    // Whatever was recorded is kept, ended as it should be; there is nobody
    // left to tell when that fails.
    if (!m_finished && m_encoder)
        std::ignore = finish();
}

std::expected<void, QString> Writer::write(std::span<const qint16> samples) {
    if (m_finished)
        return std::unexpected(Failure(Failure::Code::RecordingEnded).text());
    m_taken += static_cast<std::int64_t>(samples.size());
    std::ranges::copy(samples, std::back_inserter(m_left));
    qsizetype at = 0;
    for (; m_left.size() - at >= frameSamples; at += frameSamples)
        if (auto encoded =
                encode(std::span<const qint16>(m_left.constData(), static_cast<std::size_t>(m_left.size())).subspan(at, frameSamples));
            !encoded) {
            m_left.remove(0, at);
            return encoded;
        }
    m_left.remove(0, at);
    return {};
}

std::expected<void, QString> Writer::encode(std::span<const qint16> frame) {
    std::array<unsigned char, maxPacket> packet{};
    const opus_int32 size = opus_encode(m_encoder.get(), frame.data(), frameSamples, packet.data(), maxPacket);
    if (size < 0)
        return std::unexpected(Failure(Failure::Code::SoundUnencoded, {QString::fromUtf8(opus_strerror(size))}).text());
    m_encoded += frameSamples;
    // The last page says where the sound really ends: the encoder's delay
    // and the silence that filled the last frame are left out.
    const std::int64_t granule = m_finished ? std::min(m_encoded, m_preskip + m_taken) : m_encoded;
    const QByteArrayView bytes(packet.data(), size);
    if (auto added = m_ogg.add(bytes, granule); !added)
        return added;
    if (m_ogg.waiting() >= framesAPage && !m_finished) {
        if (auto written = m_ogg.flush(); !written)
            return written;
        m_file.flush();
    }
    return {};
}

std::expected<void, QString> Writer::finish() {
    if (m_finished)
        return {};
    m_finished = true;
    // What is still in the encoder comes out after its delay: silence
    // pushes it through, to the end of a frame.
    const std::int64_t wanted = m_preskip + m_taken;
    m_left.resize(m_left.size() + m_preskip, 0);
    for (qsizetype at = 0; m_encoded < wanted; at += frameSamples) {
        if (m_left.size() - at < frameSamples)
            m_left.resize(at + frameSamples, 0);
        if (auto encoded =
                encode(std::span<const qint16>(m_left.constData(), static_cast<std::size_t>(m_left.size())).subspan(at, frameSamples));
            !encoded)
            return encoded;
    }
    m_left.clear();
    if (auto written = m_ogg.flush(true); !written)
        return written;
    m_file.close();
    return {};
}

} // namespace Opus
