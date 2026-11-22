#pragma once

#include <QUrl>

#include "aiendpoint.h"
#include "voice.h"

// OpenAI's voices (#147, #169): its /audio/speech for the models that still
// have it (ServerVoice, which gives way to its default when the one chosen is
// retired, #162), its Realtime API for the models that speak only there
// (RealtimeVoice). When OpenAI says /audio/speech's model is gone too, as it
// will be for every one of them on 2027-01-06, the same sentence is read by
// their successor over the Realtime API, and modelRetired() says which gave
// way, so the next voice is made with the successor at once.
class OpenAiVoice : public Voice {
    Q_OBJECT

public:
    // OpenAI's server (its address and key), where its Realtime API is, and the model chosen.
    OpenAiVoice(AiServer server, QUrl realtime, const QString &model, QObject *parent = nullptr);

    void say(const QString &sentence, const Speech &how) override;
    void stop() override;
    [[nodiscard]] QString name() const override { return m_server.name; }
    // Whether it reads through the Realtime API now.
    [[nodiscard]] bool realtime() const noexcept { return m_realtime; }

private:
    // Reads with `voice` from now on (a child of this one); the one before goes.
    void use(Voice *voice, bool realtime);
    void failedWith(const Failure &why);

    AiServer m_server;
    QUrl m_address;
    Voice *m_voice = nullptr; // a child, so owned by this one
    bool m_realtime = false;
    QString m_sentence;
    Speech m_how;
};
