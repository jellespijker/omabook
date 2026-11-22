#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>
#include <utility>

#include "audioports.h"
#include "audiorecorder.h"

// The sound a program of Omabook's has to work with, given to the plugins
// (ScriptServices::audio): where a recording listens and a player plays (the
// desktop's microphone and speakers, or the tests' stand-ins), and who is
// recording now, which the window shows for as long as anyone is, with a way
// to stop it (RecordingMark.qml). Nothing records without being shown here.
// So too who reads aloud (`ai.speak`, #147): the bar over the page names the
// plugin, and it, or Shift+R, stops it.
class AudioDevices : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    // Who records, by the plugins' names.
    Q_PROPERTY(QString recorder READ recorder NOTIFY recordingChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY progressed)
    // Of the first recording: how long it is, in milliseconds, and how loud
    // it is, 0 to 1.
    Q_PROPERTY(double elapsed READ elapsed NOTIFY progressed)
    Q_PROPERTY(double level READ level NOTIFY progressed)
    // Whether a plugin reads aloud, and which, by their names.
    Q_PROPERTY(bool speaking READ speaking NOTIFY speakingChanged)
    Q_PROPERTY(QString speaker READ speaker NOTIFY speakingChanged)

public:
    using Inputs = std::function<std::unique_ptr<AudioInput>()>;
    using Outputs = std::function<std::unique_ptr<AudioOutput>()>;

    explicit AudioDevices(Inputs inputs = AudioPorts::microphone, Outputs outputs = AudioPorts::speakers, QObject *parent = nullptr);

    [[nodiscard]] std::unique_ptr<AudioInput> microphone() const { return m_inputs(); }
    // Where recordings listen from now on: a test's microphone (TestAudio) in
    // place of the desktop's, for the window's own dictation (#148) in a test.
    void setInputs(Inputs inputs) { m_inputs = std::move(inputs); }
    [[nodiscard]] std::unique_ptr<AudioOutput> speakers() const { return m_outputs(); }

    // A recording began, by `who`, and ended; told as it goes on.
    void began(AudioRecorder *recorder, const QString &who);
    void ended(const AudioRecorder *recorder);
    void tick() { emit progressed(); }

    [[nodiscard]] bool recording() const { return !m_recordings.isEmpty(); }
    [[nodiscard]] QString recorder() const;
    [[nodiscard]] bool paused() const;
    [[nodiscard]] double elapsed() const;
    [[nodiscard]] double level() const;

    // The window's stop: every recording ends, and its plugin is told.
    Q_INVOKABLE void stopRecording() { emit stopAsked(); }

    // `who` (a plugin's, named `name`) began reading aloud, and ended.
    void beganSpeaking(const QObject *who, const QString &name);
    void endedSpeaking(const QObject *who);
    [[nodiscard]] bool speaking() const { return !m_speakers.isEmpty(); }
    [[nodiscard]] QString speaker() const;
    // The window's stop: whoever reads aloud stops, and its plugin is told.
    Q_INVOKABLE void stopSpeaking() { emit stopSpeakingAsked(); }

signals:
    void recordingChanged();
    void progressed();
    void stopAsked();
    void speakingChanged();
    void stopSpeakingAsked();

private:
    struct Recording {
        QPointer<AudioRecorder> recorder;
        QString who;
    };

    Inputs m_inputs;
    Outputs m_outputs;
    QList<Recording> m_recordings;
    struct Speaker {
        QPointer<const QObject> who;
        QString name;
    };
    QList<Speaker> m_speakers;
};
