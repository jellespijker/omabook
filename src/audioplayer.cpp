#include "audioplayer.h"

#include <QList>
#include <algorithm>
#include <cstring>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] std::chrono::milliseconds timeOf(std::int64_t samples) {
    return std::chrono::milliseconds(samples * 1000 / Opus::sampleRate);
}

} // namespace

// The recording's sound as bytes, decoded as the output asks for them.
class AudioPlayer::Feed final : public QIODevice {
public:
    // Unbuffered: what was read ahead of a seek would be played after it.
    explicit Feed(Opus::Reader &reader) : m_reader(reader) { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }

    bool isSequential() const override { return true; }
    bool atEnd() const override { return m_reader.position() >= m_reader.length() && QIODevice::bytesAvailable() == 0; }
    qint64 bytesAvailable() const override { return (m_reader.length() - m_reader.position()) * 2 + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 most) override {
        QList<qint16> samples(static_cast<qsizetype>(most / 2));
        const qsizetype read = m_reader.read(std::span<qint16>(samples.data(), static_cast<std::size_t>(samples.size())));
        std::memcpy(data, samples.constData(), static_cast<std::size_t>(read) * 2);
        return read * 2;
    }
    qint64 writeData(const char * /*data*/, qint64 /*size*/) override { return -1; }

private:
    Opus::Reader &m_reader;
};

std::expected<std::unique_ptr<AudioPlayer>, QString> AudioPlayer::open(const QString &file, std::unique_ptr<AudioOutput> output) {
    auto reader = Opus::Reader::open(file);
    if (!reader)
        return std::unexpected(reader.error());
    return std::make_unique<AudioPlayer>(Key{}, std::move(*reader), std::move(output));
}

AudioPlayer::AudioPlayer(Key /*unused*/, std::unique_ptr<Opus::Reader> reader, std::unique_ptr<AudioOutput> output)
    : m_reader(std::move(reader)), m_feed(std::make_unique<Feed>(*m_reader)), m_output(std::move(output)) {
    connect(m_output.get(), &AudioOutput::drained, this, [this] {
        m_output->stop();
        m_started = m_playing = false;
        m_base = duration();
        emit ended();
    });
    connect(m_output.get(), &AudioOutput::failed, this, [this](const QString &why) {
        stop();
        emit failed(why);
    });
}

AudioPlayer::~AudioPlayer() {
    if (m_started)
        m_output->stop();
}

std::chrono::milliseconds AudioPlayer::position() const {
    if (!m_started)
        return m_base;
    return std::min(duration(), m_base + std::chrono::duration_cast<std::chrono::milliseconds>(m_output->played()));
}

std::expected<void, QString> AudioPlayer::play() {
    if (m_playing)
        return {};
    if (m_started) {
        m_output->resume();
        m_playing = true;
        return {};
    }
    if (m_reader->position() >= m_reader->length())
        m_reader->seek(0);
    m_base = timeOf(m_reader->position());
    if (auto started = m_output->start(*m_feed); !started)
        return started;
    m_started = m_playing = true;
    return {};
}

void AudioPlayer::pause() {
    if (!m_playing)
        return;
    m_output->suspend();
    m_playing = false;
}

std::expected<void, QString> AudioPlayer::seek(std::chrono::milliseconds at) {
    const bool wasPlaying = m_playing;
    if (m_started)
        m_output->stop();
    m_started = m_playing = false;
    at = std::clamp(at, std::chrono::milliseconds(0), duration());
    m_reader->seek(at.count() * Opus::sampleRate / 1000);
    m_base = at;
    return wasPlaying ? play() : std::expected<void, QString>{};
}

void AudioPlayer::stop() {
    const std::chrono::milliseconds at = position();
    if (m_started)
        m_output->stop();
    m_started = m_playing = false;
    m_base = at;
    m_reader->seek(at.count() * Opus::sampleRate / 1000);
}
