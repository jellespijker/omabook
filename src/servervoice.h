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
class ServerVoice : public Voice {
    Q_OBJECT

public:
    // The server, the model it reads with ("tts-1" unless said) and its voice ("alloy" unless said).
    ServerVoice(AiServer server, QString model, QObject *parent = nullptr);
    ~ServerVoice() override;
    ServerVoice(const ServerVoice &) = delete;
    ServerVoice &operator=(const ServerVoice &) = delete;
    ServerVoice(ServerVoice &&) = delete;
    ServerVoice &operator=(ServerVoice &&) = delete;

    static constexpr auto defaultModel = "tts-1";
    // OpenAI's own model for its voices, by its API reference of 2026-11-22.
    static constexpr auto openAiModel = "gpt-4o-mini-tts";
    // The voices OpenAI's first speech had, which servers that copy it take too.
    [[nodiscard]] static QStringList voices();
    // The voices OpenAI has now, by the same reference.
    [[nodiscard]] static QStringList openAiVoices();
    // What is asked for one sentence.
    [[nodiscard]] static QJsonObject request(const QString &model, const QString &sentence, const Speech &how);

    void say(const QString &sentence, const Speech &how) override;
    void stop() override;
    // A server by its host; OpenAI by its name.
    [[nodiscard]] QString name() const override { return m_server.preset == u"openai" ? m_server.name : m_server.host(); }

private:
    void heard(QNetworkReply *reply);

    AiServer m_server;
    QString m_model;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTemporaryDir m_folder;
    int m_files = 0;
    SoundOut m_sound;
};
