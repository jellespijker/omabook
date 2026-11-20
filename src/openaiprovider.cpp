#include "openaiprovider.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <utility>

#include "aifailures.h"

namespace {

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// A request to the server, with its key when it has one.
QNetworkRequest httpRequest(const AiServer &server, const QString &path, std::chrono::milliseconds timeout) {
    QNetworkRequest http(QUrl(server.baseUrl + path));
    http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    if (const QString key = server.currentKey(); !key.isEmpty())
        http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    http.setTransferTimeout(timeout);
    return http;
}

// Why a request to the server failed, as the other providers say it.
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

// The models in a reply to GET /models: {"data": [{"id": ...}, ...]}.
std::expected<QStringList, Failure> listedModels(const AiServer &server, QNetworkReply &reply) {
    const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject body = QJsonDocument::fromJson(reply.readAll()).object();
    if (status != 200)
        return std::unexpected(failure(server, status, reply, body, {}));
    QStringList names;
    for (const auto &model : body.value("data"_L1).toArray())
        if (const QString id = model.toObject().value("id"_L1).toString(); !id.isEmpty())
            names.append(id);
    return names;
}

} // namespace

OpenAiProvider::OpenAiProvider(AiServer server, QObject *parent) : AiProvider(parent), m_server(std::move(server)) {}

std::expected<QStringList, Failure> OpenAiProvider::listModels(const AiServer &server, std::chrono::milliseconds timeout) {
    QNetworkAccessManager network;
    QNetworkReply *reply = network.get(httpRequest(server, u"/models"_s, timeout));
    // The command line has nothing else to do meanwhile, and the window asks
    // on a thread of its own.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    return listedModels(server, *reply);
}

std::expected<QString, Failure> OpenAiProvider::defaultModel(const AiServer &server, const QStringList &listed) {
    if (!server.model.isEmpty())
        return server.model;
    const auto here = std::ranges::find_if(listed, [](const QString &model) { return !AiEndpoints::runsElsewhere(model); });
    if (here != listed.end())
        return *here;
    return std::unexpected(Failure(listed.isEmpty() ? Failure::Code::AiListsNoModels : Failure::Code::AiModelsElsewhere, {server.host()}));
}

void OpenAiProvider::ask(const AiRequest &request) {
    cancel();
    m_request = request;
    m_answered = false;
    send(modelFor(request));
}

void OpenAiProvider::send(const QString &named) {
    m_named = named;
    const QString model = named.isEmpty() ? m_server.model : named;
    if (!model.isEmpty()) {
        post(model);
        return;
    }
    // No model chosen: Chat Completions needs one, so the server is asked
    // which it has.
    m_reply = m_network.get(httpRequest(m_server, u"/models"_s, 30s));
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        if (!reply)
            return;
        m_reply = nullptr;
        reply->deleteLater();
        const auto listed = listedModels(m_server, *reply);
        const auto model = listed ? defaultModel(m_server, *listed) : std::unexpected(listed.error());
        if (!model) {
            began(m_request, {});
            emit failed(model.error());
            return;
        }
        m_server.model = *model; // asked once, for as long as this provider lives
        post(*model);
    });
}

void OpenAiProvider::post(const QString &model) {
    m_sent = model;
    began(m_request, model);
    QJsonArray messages;
    if (!m_request.system.isEmpty())
        messages.append(QJsonObject{{"role", "system"}, {"content", m_request.system}});
    for (const AiMessage &message : std::as_const(m_request.messages))
        messages.append(QJsonObject{{"role", message.role}, {"content", message.text}});
    const QJsonObject body{{"model", model}, {"stream", true}, {"messages", messages}};
    m_buffer.clear();
    m_body.clear();
    m_streamError = {};
    m_thinking = {};
    // A server loading a large model may say nothing for a while, but not for minutes.
    m_reply = m_network.post(httpRequest(m_server, u"/chat/completions"_s, 3min), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &OpenAiProvider::read);
    connect(m_reply, &QNetworkReply::finished, this, &OpenAiProvider::done);
}

void OpenAiProvider::cancel() {
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

// Server-sent events: each `data:` line is a JSON chunk of the answer, and
// `data: [DONE]` ends it.
void OpenAiProvider::read() {
    const QByteArray chunk = m_reply->readAll();
    if (m_body.size() < 65536)
        m_body += chunk; // an error arrives as a plain JSON body instead
    m_buffer += chunk;
    for (qsizetype end = m_buffer.indexOf('\n'); end >= 0; end = m_buffer.indexOf('\n')) {
        const QByteArray line = m_buffer.left(end).trimmed();
        m_buffer.remove(0, end + 1);
        if (!line.startsWith("data:"))
            continue;
        const QByteArray data = line.mid(5).trimmed();
        if (data == "[DONE]")
            continue;
        const QJsonObject event = QJsonDocument::fromJson(data).object();
        if (event.contains("error"_L1)) {
            m_streamError = event;
            continue;
        }
        // Only the answer: `reasoning_content` and `reasoning`, a thinking
        // model's working, are left unread, and so is a <think> inside it.
        const QJsonObject choice = event.value("choices"_L1).toArray().at(0).toObject();
        const QString text = m_thinking.take(choice.value("delta"_L1).toObject().value("content"_L1).toString());
        if (!text.isEmpty()) {
            m_answered = true;
            emit delta(text);
        }
    }
}

void OpenAiProvider::done() {
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    read();
    m_reply = nullptr;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool failedHere = status != 200 || reply->error() != QNetworkReply::NoError;
    if (!failedHere && m_streamError.isEmpty()) {
        if (const QString rest = m_thinking.finish(); !rest.isEmpty())
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
    const Failure problem = failure(m_server, said, *reply, failedHere ? QJsonDocument::fromJson(m_body).object() : m_streamError, m_sent);
    // The model asked for, not the one it came to: a default that was
    // refused leaves nothing to try, so this never goes round in circles.
    if (const auto next = !m_answered ? modelToRetry(m_named, problem) : std::nullopt) {
        emit noted(retryNote(m_sent, *next, m_request));
        send(*next);
        return;
    }
    emit failed(problem);
}
