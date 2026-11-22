#include "ollamaprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <bit>
#include <utility>

#include "aifailures.h"
#include "modelfit.h"
#include "openaiprovider.h"

namespace {

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

constexpr int leastRoom = 8192;  // Ollama's own is less: 2048 or 4096, by its version
constexpr int answerRoom = 4096; // for the answer, and the thinking before it; more when it runs out

QNetworkRequest httpRequest(const AiServer &server, const QString &path, std::chrono::milliseconds timeout) {
    QNetworkRequest http(QUrl(ModelFit::ollamaRoot(server) + path));
    http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    if (const QString key = server.currentKey(); !key.isEmpty())
        http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    http.setTransferTimeout(timeout);
    return http;
}

[[nodiscard]] Failure failure(const AiServer &server, int status, const QNetworkReply &reply, const QJsonObject &body,
                              const QString &model) {
    return AiFailures::ofServer({.where = server.host(),
                                 .status = status,
                                 .error = reply.error(),
                                 .errorString = reply.errorString(),
                                 .said = AiFailures::serverSaid(body),
                                 .model = model,
                                 .keySet = !server.currentKey().isEmpty()});
}

// The models in a reply to GET /api/tags: {"models": [{"name": ...}, ...]}.
std::expected<QStringList, Failure> listedModels(const AiServer &server, QNetworkReply &reply) {
    const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject body = QJsonDocument::fromJson(reply.readAll()).object();
    if (status != 200)
        return std::unexpected(failure(server, status, reply, body, {}));
    QStringList names;
    for (const auto &model : body.value("models"_L1).toArray())
        if (const QString name = model.toObject().value("name"_L1).toString(); !name.isEmpty())
            names.append(name);
    return names;
}

} // namespace

OllamaProvider::OllamaProvider(AiServer server, QObject *parent) : AiProvider(parent), m_server(std::move(server)) {}

int OllamaProvider::roomFor(const AiRequest &request) {
    qsizetype characters = request.system.size();
    for (const AiMessage &message : request.messages)
        characters += message.text.size();
    const auto needed = static_cast<unsigned>(std::min<qsizetype>(characters / 3 + answerRoom, mostRoom));
    return std::clamp(static_cast<int>(std::bit_ceil(needed)), leastRoom, mostRoom);
}

void OllamaProvider::ask(const AiRequest &request) {
    cancel();
    m_request = request;
    m_answered = false;
    m_room = roomFor(request);
    send(modelFor(request));
}

void OllamaProvider::send(const QString &named) {
    m_named = named;
    const QString model = named.isEmpty() ? m_server.model : named;
    if (!model.isEmpty()) {
        post(model);
        return;
    }
    // No model chosen: the server is asked which it has.
    m_reply = m_network.get(httpRequest(m_server, u"/api/tags"_s, 30s));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        if (!reply)
            return;
        m_reply = nullptr;
        reply->deleteLater();
        const auto listed = listedModels(m_server, *reply);
        const auto model = listed ? OpenAiProvider::defaultModel(m_server, *listed) : std::unexpected(listed.error());
        if (!model) {
            began(m_request, {});
            emit failed(model.error());
            return;
        }
        m_server.model = *model; // asked once, for as long as this provider lives
        post(*model);
    });
}

void OllamaProvider::post(const QString &model) {
    m_sent = model;
    began(m_request, model);
    QJsonArray messages;
    if (!m_request.system.isEmpty())
        messages.append(QJsonObject{{"role", "system"}, {"content", m_request.system}});
    for (const AiMessage &message : std::as_const(m_request.messages))
        messages.append(QJsonObject{{"role", message.role}, {"content", message.text}});
    QJsonObject body{{"model", model}, {"stream", true}, {"messages", messages}, {"options", QJsonObject{{"num_ctx", m_room}}}};
    if (!m_request.format.isEmpty())
        body.insert("format"_L1, m_request.format);
    m_buffer.clear();
    m_body.clear();
    m_held.clear();
    m_doneReason.clear();
    m_streamError.clear();
    m_thinking = {};
    // Loading a large model, or reading a long prompt on a small computer, may take a while.
    m_reply = m_network.post(httpRequest(m_server, u"/api/chat"_s, 5min), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &OllamaProvider::read);
    connect(m_reply, &QNetworkReply::finished, this, &OllamaProvider::done);
}

void OllamaProvider::cancel() {
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

// One JSON object to a line: {"message": {"content", "thinking"}, "done"},
// the last with "done": true and why it stopped ("done_reason").
void OllamaProvider::read() {
    const QByteArray chunk = m_reply->readAll();
    if (m_body.size() < 65536)
        m_body += chunk; // an error arrives as a plain JSON body instead
    m_buffer += chunk;
    for (qsizetype end = m_buffer.indexOf('\n'); end >= 0; end = m_buffer.indexOf('\n')) {
        const QJsonObject event = QJsonDocument::fromJson(m_buffer.left(end)).object();
        m_buffer.remove(0, end + 1);
        if (const QString error = event.value("error"_L1).toString(); !error.isEmpty())
            m_streamError = error;
        if (event.value("done"_L1).toBool())
            m_doneReason = event.value("done_reason"_L1).toString();
        // Only the answer: `thinking` is left unread, and so is a <think> inside the answer.
        const QString text = m_thinking.take(event.value("message"_L1).toObject().value("content"_L1).toString());
        if (text.isEmpty())
            continue;
        // An answer with a shape is of use only whole, and is held until it is.
        if (!m_request.format.isEmpty()) {
            m_held += text;
            continue;
        }
        m_answered = true;
        emit delta(text);
    }
}

bool OllamaProvider::askedAgainWithMoreRoom() {
    if (m_doneReason != "length"_L1 || m_request.format.isEmpty() || m_room >= mostRoom)
        return false;
    m_room *= 2;
    post(m_sent);
    return true;
}

void OllamaProvider::done() {
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    read();
    m_reply = nullptr;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool failedHere = status != 200 || reply->error() != QNetworkReply::NoError;
    if (!failedHere && m_streamError.isEmpty()) {
        if (askedAgainWithMoreRoom())
            return;
        const QString rest = m_held + m_thinking.finish();
        if (!rest.isEmpty())
            emit delta(rest);
        emit finished();
        return;
    }
    // A reply cut off on its way (a timeout, a dropped connection) is the network's doing.
    int said = status;
    if (!failedHere)
        said = 200;
    else if (status == 200)
        said = 0;
    const QJsonObject body = failedHere ? QJsonDocument::fromJson(m_body).object() : QJsonObject{{"error", m_streamError}};
    const Failure problem = failure(m_server, said, *reply, body, m_sent);
    if (const auto next = !m_answered ? modelToRetry(m_named, problem) : std::nullopt) {
        emit noted(retryNote(m_sent, *next, m_request));
        send(*next);
        return;
    }
    emit failed(problem);
}
