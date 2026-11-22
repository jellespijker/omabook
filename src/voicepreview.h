#pragma once

#include <QObject>
#include <QString>
#include <expected>
#include <functional>
#include <memory>

#include "failure.h"
#include "voice.h"

class Settings;

// A sentence read by the voice chosen for text to speech, to hear it before a
// book is (#147): the AI page's preview. It takes the voice the registry
// gives the person's own use, so it is held to the same privacy rule and AI
// Off as reading aloud, with the voice and pace chosen for that engine.
class VoicePreview : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    // Why the last one couldn't be heard; empty when it could.
    Q_PROPERTY(QString why READ why NOTIFY changed)

public:
    using VoiceMaker = std::function<std::expected<std::unique_ptr<Voice>, Failure>()>;

    explicit VoicePreview(Settings *settings, QObject *parent = nullptr);
    ~VoicePreview() override;
    VoicePreview(const VoicePreview &) = delete;
    VoicePreview &operator=(const VoicePreview &) = delete;
    VoicePreview(VoicePreview &&) = delete;
    VoicePreview &operator=(VoicePreview &&) = delete;

    // What makes the voice: the registry's, or a stand-in in the tests.
    void setVoiceMaker(VoiceMaker maker) { m_maker = std::move(maker); }

    [[nodiscard]] bool playing() const noexcept { return m_voice != nullptr; }
    [[nodiscard]] QString why() const { return m_why; }

    // Reads `text`, written in `language` ("de", as the window speaks), a
    // preview before stopped first.
    Q_INVOKABLE void play(const QString &text, const QString &language);
    Q_INVOKABLE void stop();

signals:
    void changed();

private:
    void ended(const QString &why);

    Settings *m_settings;
    VoiceMaker m_maker;
    std::unique_ptr<Voice> m_voice;
    QString m_why;
};
