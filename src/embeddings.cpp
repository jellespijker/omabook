#include "embeddings.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <utility>

#include "aifailures.h"
#include "meaningindex.h"
#include "recognition.h"
#include "secretstore.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString trimmedUrl(QString url) {
    url = url.trimmed();
    while (url.endsWith(u'/'))
        url.chop(1);
    return url;
}

// The server named for embeddings, else the AI server.
[[nodiscard]] std::expected<AiServer, QString> serverOf(const Settings &settings, const QString &omarchyProgram) {
    const QString url = trimmedUrl(settings.meaning().value("url"_L1).toString());
    if (url.isEmpty())
        return AiEndpoints::server(settings, {}, omarchyProgram);
    return AiServer{.preset = u"custom"_s,
                    .name = QUrl(url).host(),
                    .baseUrl = url,
                    .key = AiEndpoints::keptKey(url, Secrets::shared()),
                    .keyFile = {},
                    .model = {}};
}

[[nodiscard]] QStringList ownServers(const Settings &settings) {
    QStringList own;
    for (const auto &address : settings.recognition().value("own"_L1).toArray())
        own << address.toString();
    return own;
}

// The vectors of an answer: OpenAI's {"data": [{"index", "embedding"}]}, in
// the order of their index, or Ollama's own {"embeddings": [[...]]}.
[[nodiscard]] Embeddings::Vectors vectorsOf(const QJsonObject &body) {
    QList<QPair<int, QJsonArray>> listed;
    const QJsonArray data = body.value("data"_L1).toArray();
    for (qsizetype at = 0; at < data.size(); ++at) {
        const QJsonObject one = data.at(at).toObject();
        listed.append({one.value("index"_L1).toInt(static_cast<int>(at)), one.value("embedding"_L1).toArray()});
    }
    const QJsonArray own = body.value("embeddings"_L1).toArray();
    for (qsizetype at = 0; listed.isEmpty() && at < own.size(); ++at)
        listed.append({static_cast<int>(at), own.at(at).toArray()});
    std::ranges::sort(listed, {}, &QPair<int, QJsonArray>::first);
    Embeddings::Vectors vectors;
    for (const auto &[index, numbers] : std::as_const(listed)) {
        MeaningIndex::Vector vector;
        vector.reserve(static_cast<size_t>(numbers.size()));
        for (const auto &number : numbers)
            vector.push_back(static_cast<float>(number.toDouble()));
        MeaningIndex::normalize(vector);
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

} // namespace

std::expected<Embeddings::Route, Failure> Embeddings::route(const Settings &settings, const QString &omarchyProgram) {
    const auto server = serverOf(settings, omarchyProgram);
    if (!server)
        return std::unexpected(Failure::said(server.error()));
    const QString chosen = settings.meaning().value("model"_L1).toString().trimmed();
    const QString model = chosen.isEmpty() ? QString::fromLatin1(defaultModel) : chosen;
    const bool elsewhere = AiEndpoints::runsElsewhere(model);
    const bool hosted = elsewhere || Recognition::reach(server->baseUrl, ownServers(settings)) == Recognition::Reach::Hosted;
    if (hosted && !settings.meaningHosted())
        return std::unexpected(Failure(Failure::Code::MeaningHosted, {elsewhere ? model : server->host()}));
    return Route{.server = *server, .model = model, .hosted = hosted};
}

QString Embeddings::asked(const QString &model, const QString &text, Use use) {
    // nomic-embed-text is taught with these in front, and finds less without them.
    if (model.startsWith("nomic-embed"_L1))
        return (use == Use::Question ? u"search_query: "_s : u"search_document: "_s) + text;
    return text;
}

std::expected<Embeddings::Vectors, Failure> Embeddings::embed(const Route &route, const QStringList &texts, Use use,
                                                              std::chrono::milliseconds timeout) {
    if (texts.isEmpty())
        return Vectors{};
    QJsonArray input;
    for (const QString &text : texts)
        input.append(asked(route.model, text, use));
    QNetworkRequest http(QUrl(route.server.baseUrl + u"/embeddings"_s));
    http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    if (const QString key = route.server.currentKey(); !key.isEmpty())
        http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    http.setTransferTimeout(timeout);
    QNetworkAccessManager network;
    QNetworkReply *reply = network.post(http, QJsonDocument(QJsonObject{{"model", route.model}, {"input", input}}).toJson());
    // On a thread of its own, or the command line's, which has nothing else to do meanwhile.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    if (status != 200)
        return std::unexpected(AiFailures::ofServer({.where = route.server.host(),
                                                     .status = status,
                                                     .error = reply->error(),
                                                     .errorString = reply->errorString(),
                                                     .said = AiFailures::serverSaid(body),
                                                     .model = route.model,
                                                     .keySet = !route.server.currentKey().isEmpty()}));
    Vectors vectors = vectorsOf(body);
    const bool whole = std::cmp_equal(vectors.size(), texts.size()) && !vectors.front().empty()
                       && std::ranges::all_of(vectors, [&vectors](const auto &one) { return one.size() == vectors.front().size(); });
    if (!whole)
        return std::unexpected(Failure(Failure::Code::MeaningNoVectors, {route.server.host(), route.model}));
    return vectors;
}
