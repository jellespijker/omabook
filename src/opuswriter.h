#pragma once

#include <QDateTime>
#include <QFile>
#include <QList>
#include <QString>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include "oggpages.h"

struct OpusEncoder;

// Sound as Opus in Ogg (RFC 7845), the compact format recordings are kept
// in: mono at 48 kHz, about 4 KB a second of speech, so two hours are some
// 30 MB. These are its numbers, for the writer, the reader and the devices.
namespace Opus {

constexpr int sampleRate = 48000;
// 20 ms, what the encoder takes at a time.
constexpr int frameSamples = 960;

// Writes a recording as it is made: what is heard goes in as it comes, and a
// page reaches the disk every second, so a recording whose writing is cut
// short (the computer went off) is whole up to its last second.
class Writer {
    struct Key {};

public:
    struct Options {
        QString path;
        QDateTime started; // kept in the file as its DATE
        int bitrate = 32000;
    };

    [[nodiscard]] static std::expected<std::unique_ptr<Writer>, QString> open(const Options &options);
    // Through open() only.
    explicit Writer(Key /*unused*/) : m_ogg(m_file, 0x4f6d6162U) {}
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    Writer(Writer &&) = delete;
    Writer &operator=(Writer &&) = delete;

    // Mono 16-bit samples at 48 kHz, as many as there are: each whole 20 ms
    // is encoded, the rest kept for the next.
    [[nodiscard]] std::expected<void, QString> write(std::span<const qint16> samples);
    // Encodes what is left, padded with silence the file says to leave out,
    // and ends the stream. Once; the destructor does it when nobody did.
    [[nodiscard]] std::expected<void, QString> finish();

    // The samples taken so far, and the bytes written.
    [[nodiscard]] std::int64_t samples() const noexcept { return m_taken; }
    [[nodiscard]] qint64 bytes() const { return m_file.size(); }

private:
    struct Free {
        void operator()(OpusEncoder *encoder) const;
    };

    [[nodiscard]] std::expected<void, QString> begin(const Options &options);
    [[nodiscard]] std::expected<void, QString> encode(std::span<const qint16> frame);

    QFile m_file;
    Ogg::Writer m_ogg;
    std::unique_ptr<OpusEncoder, Free> m_encoder;
    QList<qint16> m_left;
    int m_preskip = 0;
    std::int64_t m_taken = 0;   // samples given to write
    std::int64_t m_encoded = 0; // samples encoded, the encoder's delay included
    bool m_finished = false;
};

} // namespace Opus
