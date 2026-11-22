#include "audioports.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <algorithm>
#include <cstring>

#include "failure.h"
#include "opuswriter.h"
#include "sounddevices.h"

using namespace Qt::StringLiterals;

namespace {

// What the microphone gives, kept as the one device its listener reads from
// while the input moves from one device to another (#173).
class Heard final : public QIODevice {
public:
    Heard() { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
    void push(const QByteArray &bytes) {
        m_bytes += bytes;
        emit readyRead();
    }
    [[nodiscard]] bool isSequential() const override { return true; }
    [[nodiscard]] qint64 bytesAvailable() const override { return m_bytes.size() + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 most) override {
        const qint64 taken = std::min<qint64>(most, m_bytes.size());
        std::memcpy(data, m_bytes.constData(), static_cast<std::size_t>(taken));
        m_bytes.remove(0, taken);
        return taken;
    }
    qint64 writeData(const char * /*data*/, qint64 /*size*/) override { return -1; }

private:
    QByteArray m_bytes;
};

// The desktop's default microphone, followed as it changes.
class Microphone final : public AudioInput {
public:
    Microphone() {
        connect(&m_default, &DefaultDevice::moved, this, [this](const SoundDevice &to) { move(to.device); });
    }
    std::expected<QIODevice *, QString> start() override {
        const QAudioDevice device = m_default.now().device;
        if (device.isNull())
            return std::unexpected(Failure(Failure::Code::NoMicrophone).text());
        if (!listen(device))
            return std::unexpected(Failure(Failure::Code::MicrophoneUnopened).text());
        return &m_heard;
    }
    void suspend() override {
        if (m_source)
            m_source->suspend();
    }
    void resume() override {
        if (m_source)
            m_source->resume();
    }
    void stop() override {
        if (m_source)
            m_source->stop();
    }

private:
    // Listens on `device`, what it hears handed on as heard.
    [[nodiscard]] bool listen(const QAudioDevice &device) {
        m_source = std::make_unique<QAudioSource>(device, AudioPorts::format());
        connect(m_source.get(), &QAudioSource::stateChanged, this, [this](QAudio::State state) {
            if (state == QAudio::StoppedState && m_source->error() != QAudio::NoError)
                emit failed(Failure(Failure::Code::MicrophoneStopped).text());
        });
        QIODevice *heard = m_source->start();
        if (heard == nullptr || m_source->error() != QAudio::NoError)
            return false;
        connect(heard, &QIODevice::readyRead, this, [this, heard] { m_heard.push(heard->readAll()); });
        return true;
    }
    // A new default while listening: it listens there from now on, paused if it was.
    void move(const QAudioDevice &to) {
        if (!m_source || m_source->state() == QAudio::StoppedState || to.isNull())
            return;
        const bool suspended = m_source->state() == QAudio::SuspendedState;
        m_source->disconnect(this);
        m_source->stop();
        if (!listen(to)) {
            emit failed(Failure(Failure::Code::MicrophoneStopped).text());
            return;
        }
        if (suspended)
            m_source->suspend();
    }

    DefaultDevice m_default{DefaultDevice::Way::In};
    Heard m_heard;
    std::unique_ptr<QAudioSource> m_source;
};

// The desktop's default speakers, followed as they change.
class Speakers final : public AudioOutput {
public:
    Speakers() {
        connect(&m_default, &DefaultDevice::moved, this, [this](const SoundDevice &to) { move(to.device); });
    }
    std::expected<void, QString> start(QIODevice &source) override {
        const QAudioDevice device = m_default.now().device;
        if (device.isNull())
            return std::unexpected(Failure(Failure::Code::NoSpeakers).text());
        m_source = &source;
        m_before = std::chrono::microseconds(0);
        play(device);
        if (m_sink->error() == QAudio::OpenError)
            return std::unexpected(Failure(Failure::Code::SpeakersUnopened).text());
        return {};
    }
    void suspend() override {
        if (m_sink)
            m_sink->suspend();
    }
    void resume() override {
        if (m_sink)
            m_sink->resume();
    }
    void stop() override {
        // Kept until the next start: this may be called from the sink's own signal.
        if (m_sink)
            m_sink->stop();
    }
    std::chrono::microseconds played() const override {
        return m_before + (m_sink ? std::chrono::microseconds(m_sink->processedUSecs()) : std::chrono::microseconds(0));
    }

private:
    // Plays the source on `device`, from where it is.
    void play(const QAudioDevice &device) {
        m_sink = std::make_unique<QAudioSink>(device, AudioPorts::format());
        connect(m_sink.get(), &QAudioSink::stateChanged, this, [this](QAudio::State state) {
            // Idle with nothing more to give is the end; idle while the
            // source still has sound is the device running ahead of it.
            if (state == QAudio::IdleState && m_source->atEnd())
                emit drained();
            else if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError && m_sink->error() != QAudio::UnderrunError)
                emit failed(Failure(Failure::Code::SoundUnplayed).text());
        });
        m_sink->start(m_source);
    }
    // A new default while playing: it goes on there from where it was, what
    // the old device still held unheard skipped, paused if it was.
    void move(const QAudioDevice &to) {
        if (!m_sink || m_source == nullptr || m_sink->state() == QAudio::StoppedState || m_source->atEnd() || to.isNull())
            return;
        const bool suspended = m_sink->state() == QAudio::SuspendedState;
        m_before += std::chrono::microseconds(m_sink->processedUSecs());
        m_sink->disconnect(this);
        m_sink->stop();
        play(to);
        if (m_sink->error() == QAudio::OpenError) {
            emit failed(Failure(Failure::Code::SoundUnplayed).text());
            return;
        }
        if (suspended)
            m_sink->suspend();
    }

    DefaultDevice m_default{DefaultDevice::Way::Out};
    QIODevice *m_source = nullptr;
    std::chrono::microseconds m_before{0};
    std::unique_ptr<QAudioSink> m_sink;
};

} // namespace

namespace AudioPorts {

QAudioFormat format() {
    QAudioFormat format;
    format.setSampleRate(Opus::sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

std::unique_ptr<AudioInput> microphone() { return std::make_unique<Microphone>(); }
std::unique_ptr<AudioOutput> speakers() { return std::make_unique<Speakers>(); }

} // namespace AudioPorts
