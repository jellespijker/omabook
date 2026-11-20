#pragma once

#include <QDateTime>
#include <QFile>
#include <QList>
#include <QString>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include "oggpages.h"
#include "opuswriter.h"

struct OpusDecoder;

namespace Opus {

// Reads a recording back as mono 16-bit samples at 48 kHz, a page at a
// time, from anywhere in it: a seek looks in the map of its pages, made once
// from their headers, and decodes from a little before the place so that
// the sound there is as it was. A recording cut short is read to its last
// whole page.
class Reader {
    struct Key {};

public:
    [[nodiscard]] static std::expected<std::unique_ptr<Reader>, QString> open(const QString &path);
    // Through open() only.
    explicit Reader(Key /*unused*/) : m_pages(m_file) {}
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    Reader(Reader &&) = delete;
    Reader &operator=(Reader &&) = delete;

    // How many samples it holds, and as a time.
    [[nodiscard]] std::int64_t length() const noexcept { return m_end - m_preskip; }
    [[nodiscard]] std::chrono::milliseconds duration() const { return std::chrono::milliseconds(length() * 1000 / sampleRate); }
    // When it was started, as its DATE says; invalid when it says nothing.
    [[nodiscard]] QDateTime started() const { return m_started; }

    // The next samples into `out`, as many as fit; fewer only at the end.
    [[nodiscard]] qsizetype read(std::span<qint16> out);
    // From this sample on; one past the end is the end.
    void seek(std::int64_t sample);
    // The sample read next.
    [[nodiscard]] std::int64_t position() const noexcept { return m_position; }

private:
    struct Free {
        void operator()(OpusDecoder *decoder) const;
    };

    [[nodiscard]] std::expected<void, QString> begin(const QString &path);
    [[nodiscard]] std::expected<void, QString> readHeaders(const QString &path);
    // Decodes the next page into m_pcm; false at the end.
    bool decodePage();
    void fillGapBefore(const Ogg::Page &page);
    void decode(const QByteArray &packet);
    // `count` samples at m_granule, as much of them as is wanted: of `pcm`,
    // or silence when it is empty.
    void place(std::span<const qint16> pcm, std::int64_t count);

    QFile m_file;
    Ogg::Reader m_pages;
    std::unique_ptr<OpusDecoder, Free> m_decoder;
    QList<Ogg::Mark> m_map; // the pages of sound, after the two of the headers
    QDateTime m_started;
    int m_preskip = 0;
    std::int64_t m_end = 0;      // the granule where the sound ends
    std::int64_t m_granule = 0;  // where the next packet decoded begins
    std::int64_t m_skip = 0;     // the granule before which what is decoded is left out
    std::int64_t m_position = 0; // the sample read next
    QByteArray m_carried;        // a packet begun on the page before
    QList<qint16> m_pcm;
    qsizetype m_pcmAt = 0;
    bool m_ended = false;
};

} // namespace Opus
