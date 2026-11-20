#pragma once

#include <QAudioFormat>
#include <QIODevice>
#include <QObject>
#include <QString>
#include <chrono>
#include <expected>
#include <memory>

// Where sound comes in and goes out: the microphone and the speakers through
// Qt Multimedia (on Omarchy, PipeWire), or stand-ins in the tests, which
// have no sound card. Both speak mono 16-bit samples at 48 kHz, what a
// recording is kept as (opuswriter.h), and the desktop's sound server turns
// that into what the device takes.
class AudioInput : public QObject {
    Q_OBJECT

public:
    // Starts listening: what is heard can be read from the device answered,
    // which says readyRead as it comes. It stays this input's.
    [[nodiscard]] virtual std::expected<QIODevice *, QString> start() = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;

signals:
    // The device went away, or stopped giving anything.
    void failed(const QString &why);
};

class AudioOutput : public QObject {
    Q_OBJECT

public:
    // Plays what `source` gives, from its start, taking it as the device
    // wants it. `source` outlives the playing.
    [[nodiscard]] virtual std::expected<void, QString> start(QIODevice &source) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    // How much has been played since start.
    [[nodiscard]] virtual std::chrono::microseconds played() const = 0;

signals:
    // `source` gave all it had, and it was played.
    void drained();
    void failed(const QString &why);
};

namespace AudioPorts {

// Mono 16-bit samples at 48 kHz.
[[nodiscard]] QAudioFormat format();
// The desktop's default microphone and speakers.
[[nodiscard]] std::unique_ptr<AudioInput> microphone();
[[nodiscard]] std::unique_ptr<AudioOutput> speakers();

} // namespace AudioPorts
