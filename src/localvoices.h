#pragma once

#include <QAudioOutput>
#include <QMediaPlayer>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "voice.h"

// A sound file played to the desktop's speakers, once, saying when it ended:
// how a sentence Piper or a server made is heard.
class SoundOut : public QObject {
    Q_OBJECT

public:
    explicit SoundOut(QObject *parent = nullptr);
    void play(const QString &file);
    void stop();

signals:
    void ended();
    void failed(const QString &why);

private:
    QAudioOutput m_output;
    QMediaPlayer m_player;
};

// Piper (#73), the neural voice that runs on this computer: a sentence is
// written to a WAV file by its command line and played. Arch has it as
// `piper-tts` (the AUR's piper-tts or piper-tts-bin), its voices as
// piper-voices-<language> in /usr/share/piper-voices; elsewhere it is `piper`,
// and voices downloaded by hand go in ~/.local/share/piper-voices. Piper
// reads its model each time it starts, so the next sentence is made while
// this one is heard.
class PiperVoice : public Voice {
    Q_OBJECT

public:
    // The command, and the voices (*.onnx models) to choose from.
    PiperVoice(QString program, QStringList models, QObject *parent = nullptr);
    ~PiperVoice() override;
    PiperVoice(const PiperVoice &) = delete;
    PiperVoice &operator=(const PiperVoice &) = delete;
    PiperVoice(PiperVoice &&) = delete;
    PiperVoice &operator=(PiperVoice &&) = delete;

    // The command found on the path, Arch's name first; empty when there is none.
    [[nodiscard]] static QString findProgram();
    // Every voice in the person's data and the system's, by name.
    [[nodiscard]] static QStringList findModels();
    // The voice a sentence is read in: the one asked for, by its name
    // ("en_US-lessac-medium"), else the first for the book's language, else the first.
    [[nodiscard]] static QString modelFor(const QStringList &models, const Speech &how);
    // Its command line for one sentence, read from its input, written to `wav`.
    [[nodiscard]] static QStringList arguments(const QString &model, const Speech &how, const QString &wav);

    void say(const QString &sentence, const Speech &how) override;
    void prepare(const QString &sentence, const Speech &how) override;
    void stop() override;
    [[nodiscard]] QString name() const override { return QStringLiteral("Piper"); }

private:
    // A sentence being made into a sound file, or made.
    struct Making {
        QString sentence;
        Speech how;
        QString wav;
        QPointer<QProcess> process;
        bool made = false;
    };
    [[nodiscard]] std::unique_ptr<Making> make(const QString &sentence, const Speech &how);
    void made(Making *making, int exitCode);
    void drop(std::unique_ptr<Making> &making);

    QString m_program;
    QStringList m_models;
    QTemporaryDir m_folder;
    int m_files = 0;
    std::unique_ptr<Making> m_now;
    std::unique_ptr<Making> m_next;
    SoundOut m_sound;
};

// speech-dispatcher (#73), the desktop's speech service, through `spd-say`:
// what reads aloud where Piper isn't installed, in whichever synthesiser it
// is set up with (espeak-ng unless changed). Its voices are its kinds.
class SpeechDispatcherVoice : public Voice {
    Q_OBJECT

public:
    explicit SpeechDispatcherVoice(QString program, QObject *parent = nullptr);
    ~SpeechDispatcherVoice() override;
    SpeechDispatcherVoice(const SpeechDispatcherVoice &) = delete;
    SpeechDispatcherVoice &operator=(const SpeechDispatcherVoice &) = delete;
    SpeechDispatcherVoice(SpeechDispatcherVoice &&) = delete;
    SpeechDispatcherVoice &operator=(SpeechDispatcherVoice &&) = delete;

    static constexpr auto program = "spd-say";
    // The kinds of voice it knows (`spd-say -t`).
    [[nodiscard]] static QStringList voices();
    // Its command line for one sentence, which returns once it was said.
    [[nodiscard]] static QStringList arguments(const QString &sentence, const Speech &how);

    void say(const QString &sentence, const Speech &how) override;
    void stop() override;
    [[nodiscard]] QString name() const override { return QStringLiteral("speech-dispatcher"); }

private:
    void drop();

    QString m_program;
    QPointer<QProcess> m_process;
};
