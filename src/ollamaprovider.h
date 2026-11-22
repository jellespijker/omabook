#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>

#include "ai.h"
#include "aiendpoint.h"
#include "thinkingfilter.h"

class QNetworkReply;

// Ollama by its own API (`/api/chat`), not the OpenAI-compatible one beside
// it, for the two things only its own has (#204). The room a request gets:
// without `num_ctx` Ollama gives a model a few thousand tokens and cuts a
// longer prompt down to its end, so a section of a book lost the
// instructions before it and a model wrote about the book instead of the
// JSON asked for. And the shape of an answer (AiRequest::format), which
// Ollama holds a model to. A thinking model's thinking comes apart
// (`thinking`, else <think> in the answer) and is never shown.
class OllamaProvider : public AiProvider {
    Q_OBJECT

public:
    // The most room asked for, in tokens; a model that has less gets its own.
    static constexpr int mostRoom = 131072;

    explicit OllamaProvider(AiServer server, QObject *parent = nullptr);

    QString name() const override { return m_server.name; }
    void ask(const AiRequest &request) override;
    void cancel() override;

    // The room a request needs, in tokens: what it sends, at three
    // characters to a token (English takes nearer four), and 4096 more for
    // the answer and any thinking before it, at least 8192. A power of two,
    // so requests of about one size share one, and Ollama keeps the model
    // loaded rather than loading it again for each.
    [[nodiscard]] static int roomFor(const AiRequest &request);

private:
    void send(const QString &named);
    void post(const QString &model);
    void read();
    void done();
    // Whether the answer is cut off at the room it had and asked again with
    // twice as much: only an answer held whole (one with a shape) can be.
    bool askedAgainWithMoreRoom();

    AiServer m_server;
    AiRequest m_request;
    QString m_named, m_sent;
    int m_room = 0;
    bool m_answered = false;
    QString m_held;       // an answer with a shape, given whole when it ends
    QString m_doneReason; // why the model stopped: "stop", or "length" when cut off
    QString m_streamError;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer, m_body;
    ThinkingFilter m_thinking;
};
