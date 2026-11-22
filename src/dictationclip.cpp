#include "dictationclip.h"

#include <QFile>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "failure.h"

namespace {

// The input gives 48 kHz (AudioPorts::format): three of its samples are one here.
constexpr int heardRate = 48000;
constexpr int step = heardRate / DictationClip::sampleRate;
constexpr qsizetype bytesPerStep = qsizetype{step} * 2;

void appendLittle(QByteArray &bytes, quint32 value, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        bytes.append(static_cast<char>((value >> (8U * i)) & 0xFFU));
}

} // namespace

std::expected<std::unique_ptr<DictationClip>, QString> DictationClip::start(std::unique_ptr<AudioInput> input) {
    if (!input)
        return std::unexpected(Failure(Failure::Code::NoMicrophone).text());
    auto clip = std::make_unique<DictationClip>(Key{}, std::move(input));
    const auto heard = clip->m_input->start();
    if (!heard) {
        clip->m_stopped = true;
        return std::unexpected(heard.error());
    }
    clip->m_heard = *heard;
    connect(*heard, &QIODevice::readyRead, clip.get(), &DictationClip::heard);
    connect(clip->m_input.get(), &AudioInput::failed, clip.get(), &DictationClip::fail);
    return clip;
}

DictationClip::DictationClip(Key /*unused*/, std::unique_ptr<AudioInput> input) : m_input(std::move(input)) {}

DictationClip::~DictationClip() { stop(); }

std::chrono::milliseconds DictationClip::elapsed() const { return std::chrono::milliseconds(m_samples.size() * 1000 / sampleRate); }

void DictationClip::heard() {
    if (m_heard == nullptr || m_stopped)
        return;
    m_carry += m_heard->readAll();
    const qsizetype steps = m_carry.size() / bytesPerStep;
    if (steps == 0)
        return;
    QList<qint16> heardSamples(steps * step);
    std::memcpy(heardSamples.data(), m_carry.constData(), static_cast<std::size_t>(steps * bytesPerStep));
    m_carry.remove(0, steps * bytesPerStep);
    int loudest = 0;
    for (qsizetype at = 0; at < heardSamples.size(); at += step) {
        // The mean of three, which keeps what is above 8 kHz from folding back into speech.
        int sum = 0;
        for (int i = 0; i < step; ++i)
            sum += heardSamples.at(at + i);
        const int sample = sum / step;
        loudest = std::max(loudest, std::abs(sample));
        m_samples.append(static_cast<qint16>(sample));
    }
    m_level = std::min(1.0, loudest / 32768.0);
    if (elapsed() >= longest) {
        stop();
        emit ended({});
    }
}

void DictationClip::stop() {
    if (m_stopped)
        return;
    heard();
    m_stopped = true;
    m_level = 0;
    m_input->stop();
}

void DictationClip::fail(const QString &why) {
    if (m_stopped)
        return;
    stop();
    emit ended(why);
}

std::expected<void, QString> DictationClip::write(const QString &file) const {
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(wav(samples(), sampleRate)) < 0 || !out.flush())
        return std::unexpected(Failure(Failure::Code::SoundFileUnwritten, {file, out.errorString()}).text());
    return {};
}

QByteArray DictationClip::wav(std::span<const qint16> samples, int rate) {
    const auto dataBytes = static_cast<quint32>(samples.size() * 2);
    QByteArray bytes;
    bytes.reserve(qsizetype{44} + qsizetype{dataBytes});
    bytes.append("RIFF");
    appendLittle(bytes, 36 + dataBytes, 4);
    bytes.append("WAVEfmt ");
    appendLittle(bytes, 16, 4);                             // the format's size
    appendLittle(bytes, 1, 2);                              // PCM
    appendLittle(bytes, 1, 2);                              // mono
    appendLittle(bytes, static_cast<quint32>(rate), 4);     // samples a second
    appendLittle(bytes, static_cast<quint32>(rate * 2), 4); // bytes a second
    appendLittle(bytes, 2, 2);                              // bytes a sample
    appendLittle(bytes, 16, 2);                             // bits a sample
    bytes.append("data");
    appendLittle(bytes, dataBytes, 4);
    for (const qint16 sample : samples)
        appendLittle(bytes, static_cast<quint16>(sample), 2);
    return bytes;
}
