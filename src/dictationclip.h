#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QtTypes>
#include <chrono>
#include <expected>
#include <memory>
#include <span>

#include "audioports.h"

// What is said for a dictation (#148), as it is heard: what an input hears,
// kept as mono 16-bit samples at 16 kHz, the WAV that every way of writing
// speech out on this computer reads as it is (voxtype, whisper.cpp) and a
// server takes; with a level, so the field can show that it hears. A
// dictation is a few sentences, so it is kept in memory until it is written
// to a file, and it ends by itself when it reaches `longest`.
class DictationClip : public QObject {
    Q_OBJECT
    struct Key {};

public:
    static constexpr int sampleRate = 16000;
    // Long enough for anything said into a note, small enough to be sent
    // whole to any server (9.6 MB).
    static constexpr std::chrono::minutes longest{5};

    // Starts listening to `input`; why not, when it can't be opened.
    [[nodiscard]] static std::expected<std::unique_ptr<DictationClip>, QString> start(std::unique_ptr<AudioInput> input);
    // Through start() only.
    DictationClip(Key /*unused*/, std::unique_ptr<AudioInput> input);
    ~DictationClip() override;
    DictationClip(const DictationClip &) = delete;
    DictationClip &operator=(const DictationClip &) = delete;
    DictationClip(DictationClip &&) = delete;
    DictationClip &operator=(DictationClip &&) = delete;

    // Stops listening; what was heard stays. Once; after that it does nothing.
    void stop();
    [[nodiscard]] bool listening() const noexcept { return !m_stopped; }
    [[nodiscard]] std::chrono::milliseconds elapsed() const;
    // How loud the last of it was, 0 to 1.
    [[nodiscard]] double level() const noexcept { return m_level; }
    [[nodiscard]] std::span<const qint16> samples() const { return {m_samples.constData(), static_cast<std::size_t>(m_samples.size())}; }
    // Writes what was heard to `file` as a WAV.
    [[nodiscard]] std::expected<void, QString> write(const QString &file) const;

    // A WAV of mono 16-bit samples at `rate`.
    [[nodiscard]] static QByteArray wav(std::span<const qint16> samples, int rate);

signals:
    // It reached `longest`, or the input failed (why); it stopped listening.
    void ended(const QString &why);

private:
    void heard();
    void fail(const QString &why);

    std::unique_ptr<AudioInput> m_input;
    QIODevice *m_heard = nullptr; // the input's
    QByteArray m_carry;           // what is short of a whole sample at 16 kHz
    QList<qint16> m_samples;
    double m_level = 0;
    bool m_stopped = false;
};
