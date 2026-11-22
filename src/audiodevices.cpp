#include "audiodevices.h"

#include <QStringList>
#include <utility>

AudioDevices::AudioDevices(Inputs inputs, Outputs outputs, QObject *parent)
    : QObject(parent), m_inputs(std::move(inputs)), m_outputs(std::move(outputs)) {}

void AudioDevices::began(AudioRecorder *recorder, const QString &who) {
    m_recordings.append({.recorder = recorder, .who = who});
    emit recordingChanged();
    emit progressed();
}

void AudioDevices::ended(const AudioRecorder *recorder) {
    const qsizetype before = m_recordings.size();
    m_recordings.removeIf([recorder](const Recording &recording) { return recording.recorder.data() == recorder || !recording.recorder; });
    if (m_recordings.size() != before) {
        emit recordingChanged();
        emit progressed();
    }
}

QString AudioDevices::recorder() const {
    QStringList who;
    for (const Recording &recording : m_recordings)
        if (!who.contains(recording.who))
            who << recording.who;
    return who.join(u", ");
}

bool AudioDevices::paused() const {
    return !m_recordings.isEmpty() && m_recordings.first().recorder && m_recordings.first().recorder->paused();
}

double AudioDevices::elapsed() const {
    return !m_recordings.isEmpty() && m_recordings.first().recorder ? static_cast<double>(m_recordings.first().recorder->elapsed().count())
                                                                    : 0.0;
}

double AudioDevices::level() const {
    return !m_recordings.isEmpty() && m_recordings.first().recorder ? m_recordings.first().recorder->level() : 0.0;
}

void AudioDevices::beganSpeaking(const QObject *who, const QString &name) {
    endedSpeaking(who);
    m_speakers.append({.who = who, .name = name});
    emit speakingChanged();
}

void AudioDevices::endedSpeaking(const QObject *who) {
    const qsizetype before = m_speakers.size();
    m_speakers.removeIf([who](const Speaker &speaker) { return speaker.who.data() == who || !speaker.who; });
    if (m_speakers.size() != before)
        emit speakingChanged();
}

QString AudioDevices::speaker() const {
    QStringList names;
    for (const Speaker &speaker : m_speakers)
        if (!names.contains(speaker.name))
            names << speaker.name;
    return names.join(u", ");
}
