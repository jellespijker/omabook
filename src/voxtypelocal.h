#pragma once

#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <memory>

#include "recognition.h"

// Speech written out on this computer by voxtype (#146), which Omarchy ships
// (`voxtype-bin`) for dictation, with the model it was set up with: the first
// way a recording is transcribed where the person chose no other, and nothing
// leaves the computer. `voxtype transcribe <file>` reads a WAV (resampling it
// to the 16 kHz mono it hears); a recording in another format is made one with
// ffmpeg first. It answers the text alone, with no word's moment, and says so
// (Result::timed); whisper.cpp or a transcription server gives them.
class VoxtypeLocal : public Recognition::Recognizer {
    Q_OBJECT

public:
    // The command, and the model to transcribe with (empty: voxtype's own choice).
    VoxtypeLocal(QString program, QString model, QObject *parent = nullptr);
    ~VoxtypeLocal() override;
    VoxtypeLocal(const VoxtypeLocal &) = delete;
    VoxtypeLocal &operator=(const VoxtypeLocal &) = delete;
    VoxtypeLocal(VoxtypeLocal &&) = delete;
    VoxtypeLocal &operator=(VoxtypeLocal &&) = delete;

    // The command run when the settings name none.
    static constexpr auto defaultProgram = "voxtype";
    // Why it can't be used here, in plain words; empty when it can.
    [[nodiscard]] static QString missing(const QString &program);

    [[nodiscard]] QString where() const override;
    // The model named, else "voxtype": whichever voxtype was set up with.
    [[nodiscard]] QString model() const override;
    void recognize(const Recognition::Input &input) override;
    void cancel() override;

private:
    void transcribe(const QString &wav);
    void read();

    QString m_program;
    QString m_model;
    QString m_language;
    QString m_recording; // its file's name, for a failure to name it
    std::unique_ptr<QTemporaryDir> m_folder;
    QProcess m_process;
    bool m_converting = false;
};

namespace Voxtype {
// What `voxtype transcribe` prints, as a result: it says what it is doing
// line by line, then, after an empty line, the text. Without that (it heard
// no speech) the text is empty. One segment, with no words.
[[nodiscard]] Recognition::Result transcript(const QString &output);
} // namespace Voxtype
