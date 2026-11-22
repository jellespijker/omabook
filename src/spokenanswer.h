#pragma once

#include <QObject>
#include <QString>
#include <expected>
#include <functional>
#include <memory>

#include "failure.h"
#include "voice.h"

class AudioDevices;
class Narration;
class Settings;

// An AI's answer read aloud (#149): the companion's, or one about the
// library, to a question that was asked by voice. It is read a sentence at a
// time by the voice chosen for text to speech, as a book is (Narration), so
// the registry's privacy rule and AI Off hold for it as for a book: a local
// voice reads, a hosted one only where it is allowed. While it reads, the
// window says who reads (AudioDevices, as for a plugin's `ai.speak`), and
// the bar's stop, Shift+R or stop() stops it; a new answer, or the person
// asking again, stops the one before.
class SpokenAnswer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool speaking READ speaking NOTIFY changed)

public:
    using VoiceMaker = std::function<std::expected<std::unique_ptr<Voice>, Failure>()>;

    SpokenAnswer(Settings *settings, AudioDevices *devices, QObject *parent = nullptr);
    ~SpokenAnswer() override;
    SpokenAnswer(const SpokenAnswer &) = delete;
    SpokenAnswer &operator=(const SpokenAnswer &) = delete;
    SpokenAnswer(SpokenAnswer &&) = delete;
    SpokenAnswer &operator=(SpokenAnswer &&) = delete;

    // What makes the voice: the registry's, or a stand-in in the tests.
    void setVoiceMaker(VoiceMaker maker) { m_maker = std::move(maker); }

    [[nodiscard]] bool speaking() const noexcept { return m_narration != nullptr; }

    // Reads `answer` (Markdown, as an answer is written) aloud, by `who` (the
    // AI's name, as the window shows it), in `language` ("de", as the window
    // speaks); one being read is stopped first. failed() says when it can't.
    Q_INVOKABLE void say(const QString &answer, const QString &who, const QString &language);
    // Stops at once; stopped is not a failure.
    Q_INVOKABLE void stop();

    // The words of an answer as a voice reads them: its Markdown's text,
    // without the marks around it (stars, hashes, a link's address).
    [[nodiscard]] static QString plain(const QString &markdown);

signals:
    void changed();
    // Why it can't be read, in words.
    void failed(const QString &why);

private:
    void ended();

    Settings *m_settings;
    AudioDevices *m_devices;
    VoiceMaker m_maker;
    std::unique_ptr<Narration> m_narration;
};
