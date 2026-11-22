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
#include "aifallback.h"
#include "aiprivacy.h"
#include "gemini.h"
#include "meaningindex.h"
#include "secretstore.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

// The server named for embeddings, else the AI server.
[[nodiscard]] std::expected<AiServer, QString> serverOf(const Settings &settings, const QString &omarchyProgram) {
    const QString url = AiEndpoints::apiUrl(settings.meaning().value("url"_L1).toString());
    if (url.isEmpty())
        return AiEndpoints::server(settings, {}, omarchyProgram);
    return AiServer{.preset = u"custom"_s,
                    .name = QUrl(url).host(),
                    .baseUrl = url,
                    .key = AiEndpoints::keptKey(url, Secrets::shared()),
                    .keyFile = {},
                    .model = {}};
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

// Gemini, where it is chosen for embeddings (#145): always someone else's computer.
[[nodiscard]] std::expected<Embeddings::Route, Failure> geminiRoute(const Settings &settings, AiPrivacy::HostedAsked asked) {
    switch (AiPrivacy::refusal(settings, Capability::Embeddings, true, asked)) {
    case CapabilityLack::None:
        break;
    case CapabilityLack::HostedNotChosen:
        return std::unexpected(Failure(Failure::Code::HostedNotChosen, {u"Gemini"_s}));
    default:
        return std::unexpected(Failure(Failure::Code::MeaningHosted, {u"Gemini"_s}));
    }
    const auto api = Gemini::api(settings);
    if (!api)
        return std::unexpected(api.error());
    return Embeddings::Route{
        .server = {.preset = u"gemini"_s, .name = u"Gemini"_s, .baseUrl = api->baseUrl, .key = api->key, .keyFile = {}, .model = {}},
        .model = Gemini::modelFor(settings, Capability::Embeddings),
        .hosted = true,
        .gemini = true,
        .insteadOf = {}};
}

[[nodiscard]] std::expected<Embeddings::Vectors, Failure> geminiVectors(const Embeddings::Route &route, const QStringList &texts,
                                                                        Embeddings::Use use, std::chrono::milliseconds timeout) {
    const Gemini::Api api{.baseUrl = route.server.baseUrl, .key = route.server.key};
    auto made =
        Gemini::embed(api, route.model, texts, use == Embeddings::Use::Question ? Gemini::Use::Question : Gemini::Use::Document, timeout);
    if (!made)
        return std::unexpected(made.error());
    const bool whole = !made->empty() && !made->front().empty()
                       && std::ranges::all_of(*made, [&made](const auto &one) { return one.size() == made->front().size(); });
    if (!whole)
        return std::unexpected(Failure(Failure::Code::MeaningNoVectors, {route.server.name, route.model}));
    // Gemini's are of length 1 only at their full size: each is made so here.
    for (auto &vector : *made)
        MeaningIndex::normalize(vector);
    return *std::move(made);
}

} // namespace

std::expected<Embeddings::Route, Failure> Embeddings::route(const Settings &settings, AiPrivacy::HostedAsked asked,
                                                           const QString &omarchyProgram) {
    // A server of the person's own is a model too: Off asks nothing of it.
    if (settings.aiOff())
        return std::unexpected(Failure(Failure::Code::AiOff));
    if (AiPrivacy::chosen(settings, Capability::Embeddings).value("provider"_L1).toString() == "gemini"_L1)
        return geminiRoute(settings, asked);
    const auto server = serverOf(settings, omarchyProgram);
    if (!server)
        return std::unexpected(Failure::said(server.error()));
    const QString chosen = settings.meaning().value("model"_L1).toString().trimmed();
    const QString model = chosen.isEmpty() ? QString::fromLatin1(defaultModel) : chosen;
    const bool hosted = AiPrivacy::hosted(settings, server->baseUrl, model);
    const QString where = AiEndpoints::runsElsewhere(model) ? model : server->host();
    switch (AiPrivacy::refusal(settings, Capability::Embeddings, hosted, asked)) {
    case CapabilityLack::None:
        break;
    case CapabilityLack::HostedNotChosen:
        return std::unexpected(Failure(Failure::Code::HostedNotChosen, {where}));
    default:
        return std::unexpected(Failure(Failure::Code::MeaningHosted, {where}));
    }
    // The first away: its fallback, by the same rule, with the same model.
    if (const auto instead = AiFallback::instead(settings, Capability::Embeddings, server->baseUrl, model, asked))
        return Route{.server = instead->server, .model = model, .hosted = instead->hosted, .gemini = false, .insteadOf = instead->first};
    return Route{.server = *server, .model = model, .hosted = hosted, .gemini = false, .insteadOf = {}};
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
    if (route.gemini)
        return geminiVectors(route, texts, use, timeout);
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
