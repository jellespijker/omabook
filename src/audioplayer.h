#pragma once

#include <QIODevice>
#include <QObject>
#include <QString>
#include <chrono>
#include <expected>
#include <memory>

#include "audioports.h"
#include "opusreader.h"

// A recording played back on an output: from anywhere in it, paused and
// sought while it plays, saying where it is so a view can follow along.
// Sound is decoded only as fast as the output takes it (Opus::Reader), so
// an hour of it costs no more memory than a minute.
class AudioPlayer : public QObject {
    Q_OBJECT
    struct Key {};

public:
    [[nodiscard]] static std::expected<std::unique_ptr<AudioPlayer>, QString> open(const QString &file,
                                                                                   std::unique_ptr<AudioOutput> output);
    // Through open() only.
    AudioPlayer(Key /*unused*/, std::unique_ptr<Opus::Reader> reader, std::unique_ptr<AudioOutput> output);
    ~AudioPlayer() override;
    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;
    AudioPlayer(AudioPlayer &&) = delete;
    AudioPlayer &operator=(AudioPlayer &&) = delete;

    // Plays from where it is (from the start after the end).
    [[nodiscard]] std::expected<void, QString> play();
    void pause();
    // To a moment in it, playing on if it was.
    [[nodiscard]] std::expected<void, QString> seek(std::chrono::milliseconds at);
    void stop();

    [[nodiscard]] bool playing() const noexcept { return m_playing; }
    [[nodiscard]] std::chrono::milliseconds position() const;
    [[nodiscard]] std::chrono::milliseconds duration() const { return m_reader->duration(); }
    [[nodiscard]] QDateTime started() const { return m_reader->started(); }

signals:
    // It played to its end.
    void ended();
    void failed(const QString &why);

private:
    class Feed;

    std::unique_ptr<Opus::Reader> m_reader;
    std::unique_ptr<Feed> m_feed;
    std::unique_ptr<AudioOutput> m_output;
    // Where the output started from; it says how much it played since.
    std::chrono::milliseconds m_base{0};
    bool m_playing = false;
    bool m_started = false; // the output has the feed (it may be suspended)
};
