#include "testaudio.h"

#include <QFile>
#include <QIODevice>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

constexpr int sampleRate = 48000;
// How often the stand-ins move on, and so how much at a time.
constexpr auto step = 20ms;
constexpr qsizetype bytesPerStep = sampleRate * 2 * step.count() / 1000;

// A second of an A, half as loud as a sample can be.
[[nodiscard]] QByteArray tone() {
    QByteArray bytes(sampleRate * 2, Qt::Uninitialized);
    for (int i = 0; i < sampleRate; ++i) {
        const auto sample = static_cast<qint16>(16000 * std::sin(2 * std::numbers::pi * 440 * i / sampleRate));
        qToLittleEndian(sample, bytes.data() + (2 * i));
    }
    return bytes;
}

// The samples of a WAV file, when they are mono 16-bit at 48 kHz.
[[nodiscard]] std::optional<QByteArray> wav(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 12 || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WAVE")
        return std::nullopt;
    bool fits = false;
    for (qsizetype at = 12; at + 8 <= bytes.size();) {
        const QByteArray id = bytes.mid(at, 4);
        const qsizetype size = qFromLittleEndian<quint32>(bytes.constData() + at + 4);
        const char *chunk = bytes.constData() + at + 8;
        if (id == "fmt " && size >= 16 && at + 8 + 16 <= bytes.size())
            fits = qFromLittleEndian<quint16>(chunk) == 1 && qFromLittleEndian<quint16>(chunk + 2) == 1
                   && qFromLittleEndian<quint32>(chunk + 4) == sampleRate && qFromLittleEndian<quint16>(chunk + 14) == 16;
        if (id == "data")
            return fits ? std::optional(bytes.mid(at + 8, size & ~qsizetype{1})) : std::nullopt;
        at += 8 + size + (size % 2);
    }
    return std::nullopt;
}

// What the microphone gives, as its device does: what it hears, as it hears it.
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

class Microphone final : public AudioInput {
public:
    explicit Microphone(QByteArray sound) : m_sound(std::move(sound)) {
        m_clock.setInterval(step);
        connect(&m_clock, &QTimer::timeout, this, [this] { hear(); });
    }
    std::expected<QIODevice *, QString> start() override {
        m_clock.start();
        return &m_heard;
    }
    void suspend() override { m_clock.stop(); }
    void resume() override { m_clock.start(); }
    void stop() override { m_clock.stop(); }

private:
    void hear() {
        QByteArray piece;
        while (piece.size() < bytesPerStep) {
            const qsizetype take = std::min(bytesPerStep - piece.size(), m_sound.size() - m_at);
            piece += m_sound.mid(m_at, take);
            m_at = (m_at + take) % m_sound.size();
        }
        m_heard.push(piece);
    }

    QByteArray m_sound;
    qsizetype m_at = 0;
    Heard m_heard;
    QTimer m_clock;
};

class Speakers final : public AudioOutput {
public:
    Speakers() {
        m_clock.setInterval(step);
        connect(&m_clock, &QTimer::timeout, this, [this] { take(); });
    }
    std::expected<void, QString> start(QIODevice &source) override {
        m_source = &source;
        m_played = 0;
        m_clock.start();
        return {};
    }
    void suspend() override { m_clock.stop(); }
    void resume() override { m_clock.start(); }
    void stop() override {
        m_clock.stop();
        m_source = nullptr;
    }
    [[nodiscard]] std::chrono::microseconds played() const override {
        return std::chrono::microseconds(m_played * 1'000'000 / (qint64{sampleRate} * 2));
    }

private:
    void take() {
        if (m_source == nullptr)
            return;
        m_played += m_source->read(bytesPerStep).size();
        if (m_source->atEnd()) {
            m_clock.stop();
            emit drained();
        }
    }

    QIODevice *m_source = nullptr;
    qint64 m_played = 0;
    QTimer m_clock;
};

} // namespace

std::optional<AudioDevices::Inputs> TestAudio::microphone(const QString &hears) {
    std::optional<QByteArray> sound;
    if (hears == "tone"_L1)
        sound = tone();
    else if (hears == "silence"_L1)
        sound = QByteArray(sampleRate * 2, '\0');
    else
        sound = wav(hears);
    if (!sound || sound->isEmpty())
        return std::nullopt;
    return [heard = *sound] { return std::unique_ptr<AudioInput>(std::make_unique<Microphone>(heard)); };
}

std::unique_ptr<AudioOutput> TestAudio::speakers() { return std::make_unique<Speakers>(); }
