#pragma once

#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "recognition.h"

// Speech written out on this computer by whisper.cpp (#84): the way a
// recording is transcribed when no server offers it, and nothing leaves the
// computer. Its command line (`whisper-cli`) is asked for one word per line
// with its moment (`-ml 1 -sow`) as JSON; a recording that isn't WAV is first
// made one with ffmpeg, which whisper.cpp reads best.
class WhisperLocal : public Recognition::Recognizer {
    Q_OBJECT

public:
    // The command and the model file (a ggml-*.bin).
    WhisperLocal(QString program, QString modelFile, QObject *parent = nullptr);
    ~WhisperLocal() override;
    WhisperLocal(const WhisperLocal &) = delete;
    WhisperLocal &operator=(const WhisperLocal &) = delete;
    WhisperLocal(WhisperLocal &&) = delete;
    WhisperLocal &operator=(WhisperLocal &&) = delete;

    // The command run when the settings name none.
    static constexpr auto defaultProgram = "whisper-cli";
    // Where a model is looked for when the settings name none: the first
    // ggml-*.bin, by name, in whisper.cpp's folder of the person's data
    // ($XDG_DATA_HOME/whisper.cpp/models) or the system's.
    [[nodiscard]] static QString findModel();
    // Why it can't be used here, in plain words; empty when it can.
    [[nodiscard]] static QString missing(const QString &program, const QString &modelFile);

    [[nodiscard]] QString where() const override;
    [[nodiscard]] QString model() const override;
    [[nodiscard]] bool timesWords() const override { return true; }
    void recognize(const Recognition::Input &input) override;
    void cancel() override;

private:
    void transcribe(const QString &wav);
    void read();

    QString m_program;
    QString m_modelFile;
    QString m_language;
    std::unique_ptr<QTemporaryDir> m_folder;
    QProcess m_process;
    bool m_converting = false;
};

namespace Whisper {
// whisper.cpp's JSON (`-oj`), one word to an entry (`-ml 1 -sow`), as a
// result: the words, in segments that end where a sentence does.
[[nodiscard]] Recognition::Result transcript(const QJsonObject &output);
} // namespace Whisper
