#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <chrono>
#include <expected>

#include "ai.h"
#include "aiendpoint.h"

class QNetworkReply;

// Any server that speaks the OpenAI-compatible API: Ollama, llama.cpp's
// llama-server, LM Studio, vLLM, Omarchy's local models, and the hosted ones
// (OpenAI, OpenRouter, Groq, ...). It speaks Chat Completions, the one every
// such server has, not OpenAI's newer Responses API: the system prompt goes
// first among the messages, and the answer is read as it streams. A
// reasoning model's thinking is never shown.
class OpenAiProvider : public AiProvider {
    Q_OBJECT

public:
    explicit OpenAiProvider(AiServer server, QObject *parent = nullptr);

    QString name() const override { return m_server.name; }
    void ask(const AiRequest &request) override;
    void cancel() override;

    // The models the server lists (GET /models), in its order, waiting at
    // most `timeout`. The command line waits for it; the window asks it on
    // a thread of its own. Fails, in English, saying why.
    [[nodiscard]] static std::expected<QStringList, QString> listModels(const AiServer &server,
                                                                        std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // The model asked when none is chosen: what the server is known to
    // serve, else the first it lists that runs on it (never one whose name
    // says it sends the text on elsewhere; see AiEndpoints::runsElsewhere).
    [[nodiscard]] static std::expected<QString, QString> defaultModel(const AiServer &server, const QStringList &listed);

private:
    // Asks for this model, or for the default (found first) when it is empty.
    void send(const QString &named);
    void post(const QString &model);
    void read();
    void done();

    AiServer m_server;
    AiRequest m_request;
    QString m_named; // the model asked for, empty for the default
    QString m_sent;  // the model the request in hand went to
    bool m_answered = false;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer, m_body;
    QString m_streamError;
};
