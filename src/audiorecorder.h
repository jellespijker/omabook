#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <chrono>
#include <expected>
#include <memory>

#include "audioports.h"
#include "opuswriter.h"

// A recording being made: what an input hears, written as Opus to a file as
// it comes (Opus::Writer), with its start moment, a level for a meter, and
// pauses. Each stretch between pauses is a part with the moment it began, so
// a moment in the recording can be told as a moment of the day, as the ink
// written while it ran is (#79, jellespijker/omabook-notebooks#5).
class AudioRecorder : public QObject {
    Q_OBJECT
    struct Key {};

public:
    struct Part {
        std::chrono::milliseconds at{0}; // how far into the recording it begins
        QDateTime started;
    };
    // What a recording came to.
    struct Summary {
        QString file;
        QDateTime started;
        std::chrono::milliseconds duration{0};
        qint64 bytes = 0;
        QList<Part> parts;
        QString problem; // why it ended by itself, if it did
        [[nodiscard]] QJsonObject toJson() const;
    };

    // Starts recording what `input` hears into `file`, at `now`.
    [[nodiscard]] static std::expected<std::unique_ptr<AudioRecorder>, QString>
    start(const QString &file, std::unique_ptr<AudioInput> input, const QDateTime &now);
    // Through start() only.
    AudioRecorder(Key /*unused*/, std::unique_ptr<AudioInput> input, std::unique_ptr<Opus::Writer> writer, QString file,
                  const QDateTime &now);
    ~AudioRecorder() override;
    AudioRecorder(const AudioRecorder &) = delete;
    AudioRecorder &operator=(const AudioRecorder &) = delete;
    AudioRecorder(AudioRecorder &&) = delete;
    AudioRecorder &operator=(AudioRecorder &&) = delete;

    // While paused nothing is heard; a resume begins a part.
    void pause();
    void resume(const QDateTime &now);
    // Ends it and writes the file's end. Once; after that it answers the same.
    Summary stop();

    [[nodiscard]] bool paused() const noexcept { return m_paused; }
    [[nodiscard]] bool recording() const noexcept { return !m_stopped; }
    [[nodiscard]] std::chrono::milliseconds elapsed() const;
    // How loud the last of it was, 0 to 1.
    [[nodiscard]] double level() const noexcept { return m_level; }
    [[nodiscard]] QDateTime started() const { return m_parts.first().started; }
    [[nodiscard]] QString file() const { return m_file; }

signals:
    // It ended by itself (the input failed, the disk was full); stop() says why.
    void stopped();

private:
    void heard();
    void fail(const QString &why);

    std::unique_ptr<AudioInput> m_input;
    std::unique_ptr<Opus::Writer> m_writer;
    QIODevice *m_heard = nullptr; // the input's
    QByteArray m_carry;           // half a sample, until the rest of it comes
    QString m_file, m_problem;
    QList<Part> m_parts;
    double m_level = 0;
    bool m_paused = false, m_stopped = false;
    Summary m_summary;
};
