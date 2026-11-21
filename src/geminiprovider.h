#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>

#include "ai.h"
#include "gemini.h"

class QNetworkReply;

// Gemini answering through its own API with the person's key (#145): answers
// and quick work, streamed (streamGenerateContent as server-sent events, each
// a piece of the answer), the conversation as Gemini's user and model turns
// and the instructions as its system instruction. A refused model is asked
// once more with the main one, as with every provider (modelToRetry).
class GeminiProvider : public AiProvider {
    Q_OBJECT

public:
    explicit GeminiProvider(Gemini::Api api, QObject *parent = nullptr);

    [[nodiscard]] QString name() const override;
    void ask(const AiRequest &request) override;
    void cancel() override;

private:
    void send(const QString &named);
    void read();
    void done();

    Gemini::Api m_api;
    AiRequest m_request;
    QString m_named; // the model asked for, empty for the default
    QString m_sent;  // the model the request in hand went to
    bool m_answered = false;
    bool m_declined = false;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer, m_body;
    QJsonObject m_streamError; // an error event in the stream
};
