#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTemporaryDir>

#include "aiendpoint.h"
#include "localvoices.h"
#include "voice.h"

// A server of the OpenAI-compatible API that reads aloud (#73), or OpenAI's
// own (#147): each sentence is sent to its /audio/speech, and the WAV it
// answers is played. Only a server the person chose for it, and only their
// own unless they allowed hosted ones (Voices::choose says which may be asked).
// A model the server says it has retired, or doesn't have, gives way to the
// fallback, when there is one (OpenAI's default, #162): the sentence is asked
// again with it, and modelRetired() says which gave way.
class ServerVoice : public Voice {
    Q_OBJECT

public:
    // The server, the model it reads with ("tts-1" unless said) and its voice
    // ("alloy" unless said), and the model it falls back to (none unless said).
    ServerVoice(AiServer server, QString model, QString fallback = {}, QObject *parent = nullptr);
    ~ServerVoice() override;
    ServerVoice(const ServerVoice &) = delete;
    ServerVoice &operator=(const ServerVoice &) = delete;
    ServerVoice(ServerVoice &&) = delete;
    ServerVoice &operator=(ServerVoice &&) = delete;

    static constexpr auto defaultModel = "tts-1";
    // OpenAI's own model for its voices, by its API reference of 2026-11-22.
    // Checked again on 2026-11-22 (#162), its deprecations page shuts down
    // every model of /audio/speech (tts-1, tts-1-hd, gpt-4o-mini-tts and its
    // snapshots) on 2027-01-06. The successor it names, gpt-realtime-2.1-mini,
    // is the Realtime API's (a WebSocket session), not /audio/speech's, so
    // there is no model here to move to; gpt-4o-mini-tts stays the default
    // until Omabook speaks the Realtime API, and when it is shut down the
    // voice says so (Failure::Code::VoiceModelRetired) rather than a status.
    static constexpr auto openAiModel = "gpt-4o-mini-tts";
    // The voices OpenAI's first speech had, which servers that copy it take too.
    [[nodiscard]] static QStringList voices();
    // The voices OpenAI has now, by the same reference.
    [[nodiscard]] static QStringList openAiVoices();
    // What is asked for one sentence.
    [[nodiscard]] static QJsonObject request(const QString &model, const QString &sentence, const Speech &how);
    // Whether an answer says the model asked for is retired or unknown: an
    // error naming the model, as OpenAI's "model_not_found" (its error codes
    // name none for a model shut down, so its words are read too).
    [[nodiscard]] static bool modelGone(int status, const QByteArray &answer);
    // The same of the error itself ({code, message}), as the Realtime API sends it too (#169).
    [[nodiscard]] static bool modelGone(const QJsonObject &error);

    void say(const QString &sentence, const Speech &how) override;
    void stop() override;
    // A server by its host; OpenAI by its name.
    [[nodiscard]] QString name() const override { return m_server.preset == u"openai" ? m_server.name : m_server.host(); }
    // The model it reads with now: the fallback once the one chosen gave way.
    [[nodiscard]] QString model() const { return m_model; }

private:
    void ask();
    void heard(QNetworkReply *reply);

    AiServer m_server;
    QString m_model;
    QString m_fallback;
    QString m_sentence;
    Speech m_how;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTemporaryDir m_folder;
    int m_files = 0;
    SoundOut m_sound;
};
