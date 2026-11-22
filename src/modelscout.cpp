#include "modelscout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <chrono>
#include <memory>
#include <utility>

#include "aifailures.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// More than this many models on one server are listed by their names alone.
constexpr qsizetype shownAtMost = 24;

[[nodiscard]] QStringList strings(const QJsonValue &value) {
    QStringList all;
    for (const auto &one : value.toArray())
        all.append(one.toString());
    return all;
}

// One of Ollama's /api/tags: {name, size, details: {family, families}}.
[[nodiscard]] ModelFit::Model modelOf(const QJsonObject &listed) {
    const QJsonObject details = listed.value("details"_L1).toObject();
    return {.name = listed.value("name"_L1).toString(listed.value("model"_L1).toString()),
            .family = details.value("family"_L1).toString(),
            .families = strings(details.value("families"_L1)),
            .abilities = {},
            .bytes = listed.value("size"_L1).toInteger()};
}

[[nodiscard]] Failure failureOf(const AiServer &server, const QNetworkReply &reply, const QByteArray &body, const QString &model = {}) {
    return AiFailures::ofServer({.where = server.host(),
                                 .status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                                 .error = reply.error(),
                                 .errorString = reply.errorString(),
                                 .said = AiFailures::serverSaid(QJsonDocument::fromJson(body).object()),
                                 .model = model,
                                 .keySet = !server.currentKey().isEmpty()});
}

[[nodiscard]] bool answered(const QNetworkReply &reply) {
    return reply.error() == QNetworkReply::NoError && reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
}

} // namespace

ModelScout::ModelScout(QObject *parent) : QObject(parent) {}

QNetworkReply *ModelScout::sent(const AiServer &server, const QString &url, const QByteArray &body) {
    QNetworkRequest http{QUrl(url)};
    if (const QString key = server.currentKey(); !key.isEmpty())
        http.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    // A pull says how far it is every moment; a list is answered at once.
    http.setTransferTimeout(body.isEmpty() ? 15s : 2min);
    QNetworkReply *reply = nullptr;
    if (body.isEmpty()) {
        reply = m_network.get(http);
    } else {
        http.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
        reply = m_network.post(http, body);
    }
    m_replies.removeIf([](const QPointer<QNetworkReply> &done) { return done.isNull(); });
    m_replies.append(reply);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    return reply;
}

void ModelScout::list(const AiServer &server, Listed done) {
    QNetworkReply *reply = sent(server, ModelFit::ollamaRoot(server) + u"/api/tags"_s);
    connect(reply, &QNetworkReply::finished, this, [this, reply, server, done = std::move(done), round = m_round] {
        if (round != m_round)
            return;
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        // Only Ollama answers /api/tags with its models; any other server is asked as every one is.
        if (!answered(*reply) || !body.value("models"_L1).isArray()) {
            openAiModels(server, done);
            return;
        }
        Listing listing{.ollama = true, .models = {}};
        for (const auto &one : body.value("models"_L1).toArray())
            listing.models.append(modelOf(one.toObject()));
        show(server, std::move(listing), done);
    });
}

void ModelScout::show(const AiServer &server, Listing listing, const Listed &done) {
    const qsizetype asked = std::min(listing.models.size(), shownAtMost);
    if (asked == 0) {
        done(listing);
        return;
    }
    // Each is asked what it does at once; the listing is told when the last has answered.
    const auto shared = std::make_shared<Listing>(std::move(listing));
    const auto left = std::make_shared<qsizetype>(asked);
    for (qsizetype at = 0; at < asked; ++at) {
        const QByteArray body = QJsonDocument(QJsonObject{{"model", shared->models.at(at).name}}).toJson(QJsonDocument::Compact);
        QNetworkReply *reply = sent(server, ModelFit::ollamaRoot(server) + u"/api/show"_s, body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, shared, left, at, done, round = m_round] {
            if (round != m_round)
                return;
            if (answered(*reply)) {
                const QJsonObject shown = QJsonDocument::fromJson(reply->readAll()).object();
                ModelFit::Model &model = shared->models[at];
                model.abilities = strings(shown.value("capabilities"_L1));
                const QJsonObject details = shown.value("details"_L1).toObject();
                if (model.family.isEmpty())
                    model.family = details.value("family"_L1).toString();
                if (model.families.isEmpty())
                    model.families = strings(details.value("families"_L1));
            }
            if (--*left == 0)
                done(*shared);
        });
    }
}

void ModelScout::openAiModels(const AiServer &server, Listed done) {
    QNetworkReply *reply = sent(server, server.baseUrl + u"/models"_s);
    connect(reply, &QNetworkReply::finished, this, [this, reply, server, done = std::move(done), round = m_round] {
        if (round != m_round)
            return;
        const QByteArray body = reply->readAll();
        if (!answered(*reply)) {
            done(std::unexpected(failureOf(server, *reply, body)));
            return;
        }
        Listing listing;
        for (const auto &one : QJsonDocument::fromJson(body).object().value("data"_L1).toArray())
            listing.models.append(
                {.name = one.toObject().value("id"_L1).toString(), .family = {}, .families = {}, .abilities = {}, .bytes = 0});
        done(listing);
    });
}

void ModelScout::pull(const AiServer &server, const QString &model, Progress progress, Pulled done) {
    const QByteArray body = QJsonDocument(QJsonObject{{"model", model}, {"stream", true}}).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = sent(server, ModelFit::ollamaRoot(server) + u"/api/pull"_s, body);
    // Ollama writes a line of JSON for each step: {status, total, completed}, {error}, and {status: "success"} last.
    struct Pulling {
        QByteArray buffer;
        QString error;
        bool success = false;
        Progress progress;
        void take(const QByteArray &more) {
            buffer += more;
            for (qsizetype end = buffer.indexOf('\n'); end >= 0; end = buffer.indexOf('\n')) {
                const QJsonObject line = QJsonDocument::fromJson(buffer.left(end)).object();
                buffer.remove(0, end + 1);
                if (const QString said = line.value("error"_L1).toString(); !said.isEmpty())
                    error = said;
                success = success || line.value("status"_L1).toString() == "success"_L1;
                if (const qint64 total = line.value("total"_L1).toInteger(); total > 0 && progress)
                    progress(line.value("completed"_L1).toInteger(), total);
            }
        }
    };
    const auto pulling = std::make_shared<Pulling>();
    pulling->progress = std::move(progress);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, pulling, round = m_round] {
        if (round == m_round)
            pulling->take(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, server, model, pulling, done = std::move(done), round = m_round] {
        if (round != m_round)
            return;
        const QByteArray body = reply->readAll();
        // What is left, its last line too, whether or not it ends the line.
        pulling->take(body + '\n');
        if (!answered(*reply) && pulling->error.isEmpty())
            done(std::unexpected(failureOf(server, *reply, body, model)));
        else if (!pulling->success)
            done(std::unexpected(Failure(Failure::Code::ModelNotPulled, {model, server.host(), pulling->error})));
        else
            done({});
    });
}

void ModelScout::cancel() {
    ++m_round;
    for (const QPointer<QNetworkReply> &reply : std::exchange(m_replies, {}))
        if (reply)
            reply->abort();
}
