#include "audioports.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>

#include "opuswriter.h"

using namespace Qt::StringLiterals;

namespace {

class Microphone final : public AudioInput {
public:
    std::expected<QIODevice *, QString> start() override {
        const QAudioDevice device = QMediaDevices::defaultAudioInput();
        if (device.isNull())
            return std::unexpected(u"There is no microphone."_s);
        m_source = std::make_unique<QAudioSource>(device, AudioPorts::format());
        connect(m_source.get(), &QAudioSource::stateChanged, this, [this](QAudio::State state) {
            if (state == QAudio::StoppedState && m_source->error() != QAudio::NoError)
                emit failed(u"The microphone stopped giving sound."_s);
        });
        QIODevice *heard = m_source->start();
        if (heard == nullptr || m_source->error() != QAudio::NoError)
            return std::unexpected(u"The microphone could not be opened."_s);
        return heard;
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
    std::unique_ptr<QAudioSource> m_source;
};

class Speakers final : public AudioOutput {
public:
    std::expected<void, QString> start(QIODevice &source) override {
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull())
            return std::unexpected(u"There is nothing to play sound on."_s);
        m_sink = std::make_unique<QAudioSink>(device, AudioPorts::format());
        connect(m_sink.get(), &QAudioSink::stateChanged, this, [this, &source](QAudio::State state) {
            // Idle with nothing more to give is the end; idle while the
            // source still has sound is the device running ahead of it.
            if (state == QAudio::IdleState && source.atEnd())
                emit drained();
            else if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError && m_sink->error() != QAudio::UnderrunError)
                emit failed(u"The sound could not be played."_s);
        });
        m_sink->start(&source);
        if (m_sink->error() == QAudio::OpenError)
            return std::unexpected(u"The speakers could not be opened."_s);
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
        return m_sink ? std::chrono::microseconds(m_sink->processedUSecs()) : std::chrono::microseconds(0);
    }

private:
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
