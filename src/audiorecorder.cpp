#include "audiorecorder.h"

#include <QFile>
#include <QJsonArray>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace Qt::StringLiterals;

QJsonObject AudioRecorder::Summary::toJson() const {
    QJsonArray partList;
    for (const Part &part : parts)
        partList << QJsonObject{{"at", static_cast<double>(part.at.count())},
                                {"started", part.started.toUTC().toString(Qt::ISODateWithMs)}};
    QJsonObject json{{"file", file},
                     {"started", started.toUTC().toString(Qt::ISODateWithMs)},
                     {"duration", static_cast<double>(duration.count())},
                     {"bytes", static_cast<double>(bytes)},
                     {"parts", partList}};
    if (!problem.isEmpty())
        json.insert("problem", problem);
    return json;
}

std::expected<std::unique_ptr<AudioRecorder>, QString> AudioRecorder::start(const QString &file, std::unique_ptr<AudioInput> input,
                                                                            const QDateTime &now) {
    auto writer = Opus::Writer::open({.path = file, .started = now});
    if (!writer)
        return std::unexpected(writer.error());
    auto recorder = std::make_unique<AudioRecorder>(Key{}, std::move(input), std::move(*writer), file, now);
    const auto heard = recorder->m_input->start();
    if (!heard) {
        // Nothing was heard, so there is no recording to keep.
        recorder->m_stopped = true;
        recorder->m_writer.reset();
        QFile::remove(file);
        return std::unexpected(heard.error());
    }
    recorder->m_heard = *heard;
    connect(*heard, &QIODevice::readyRead, recorder.get(), &AudioRecorder::heard);
    connect(recorder->m_input.get(), &AudioInput::failed, recorder.get(), &AudioRecorder::fail);
    return recorder;
}

AudioRecorder::AudioRecorder(Key /*unused*/, std::unique_ptr<AudioInput> input, std::unique_ptr<Opus::Writer> writer, QString file,
                             const QDateTime &now)
    : m_input(std::move(input)), m_writer(std::move(writer)), m_file(std::move(file)),
      m_parts{{.at = std::chrono::milliseconds(0), .started = now}} {}

AudioRecorder::~AudioRecorder() { stop(); }

std::chrono::milliseconds AudioRecorder::elapsed() const {
    return m_writer ? std::chrono::milliseconds(m_writer->samples() * 1000 / Opus::sampleRate) : m_summary.duration;
}

void AudioRecorder::heard() {
    if (m_heard == nullptr || m_stopped || !m_problem.isEmpty())
        return;
    m_carry += m_heard->readAll();
    const qsizetype whole = m_carry.size() / 2;
    if (whole == 0 || m_paused) {
        // While paused nothing is kept, so a resume goes on from the pause.
        if (m_paused)
            m_carry.clear();
        return;
    }
    QList<qint16> samples(whole);
    std::memcpy(samples.data(), m_carry.constData(), static_cast<std::size_t>(whole) * 2);
    m_carry.remove(0, whole * 2);
    const auto loudest = std::ranges::max(samples, {}, [](qint16 sample) { return std::abs(static_cast<int>(sample)); });
    m_level = std::min(1.0, std::abs(static_cast<int>(loudest)) / 32768.0);
    if (auto written = m_writer->write(std::span<const qint16>(samples.constData(), static_cast<std::size_t>(samples.size()))); !written)
        fail(written.error());
}

void AudioRecorder::pause() {
    if (m_stopped || m_paused)
        return;
    heard();
    m_paused = true;
    m_level = 0;
    m_input->suspend();
}

void AudioRecorder::resume(const QDateTime &now) {
    if (m_stopped || !m_paused)
        return;
    m_paused = false;
    m_parts.append({.at = elapsed(), .started = now});
    m_input->resume();
}

AudioRecorder::Summary AudioRecorder::stop() {
    if (m_stopped)
        return m_summary;
    if (!m_paused)
        heard();
    m_stopped = true;
    m_input->stop();
    m_summary = {.file = m_file, .started = started(), .duration = elapsed(), .bytes = 0, .parts = m_parts, .problem = m_problem};
    if (auto finished = m_writer->finish(); !finished && m_summary.problem.isEmpty())
        m_summary.problem = finished.error();
    m_summary.bytes = m_writer->bytes();
    m_writer.reset();
    m_level = 0;
    return m_summary;
}

void AudioRecorder::fail(const QString &why) {
    if (m_stopped)
        return;
    m_problem = why;
    stop();
    emit stopped();
}
