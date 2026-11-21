#include "kosyncclient.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <chrono>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

KosyncClient::KosyncClient(QNetworkAccessManager &network, QUrl server, QString user, QString key)
    : m_network(&network), m_server(std::move(server)), m_user(std::move(user)), m_key(std::move(key)) {}

QString KosyncClient::keyOf(const QString &password) {
    return QString::fromLatin1(QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex());
}

std::expected<QJsonObject, Failure> KosyncClient::call(const QByteArray &verb, const QString &path, const QJsonObject &body) {
    const QString where = m_server.toDisplayString();
    QUrl url = m_server;
    url.setPath(m_server.path().chopped(m_server.path().endsWith('/') ? 1 : 0) + path);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.koreader.v1+json");
    request.setRawHeader("x-auth-user", m_user.toUtf8());
    request.setRawHeader("x-auth-key", m_key.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setTransferTimeout(20s);
    const QByteArray sent = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
    const std::unique_ptr<QNetworkReply> reply(m_network->sendCustomRequest(request, verb, sent));
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401)
        return std::unexpected(Failure(Failure::Code::SyncServerRefused, {where}));
    if (status == 0)
        return std::unexpected(Failure(Failure::Code::SyncServerUnreachable, {where, reply->errorString()}));
    QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
    if (status >= 300)
        return std::unexpected(
            Failure(Failure::Code::SyncServerAnswered, {where, answer.value("message").toString(QString::number(status))}));
    return answer;
}

std::expected<void, Failure> KosyncClient::authorize() {
    const auto answer = call("GET", u"/users/auth"_s);
    if (!answer)
        return std::unexpected(answer.error());
    return {};
}

std::expected<std::optional<KoreaderRecord>, Failure> KosyncClient::progress(const QString &document) {
    const auto answer = call("GET", u"/syncs/progress/"_s + document);
    if (!answer)
        return std::unexpected(answer.error());
    return KoreaderRecord::fromJson(*answer);
}

std::expected<void, Failure> KosyncClient::put(const KoreaderRecord &record) {
    QJsonObject body = record.toJson();
    body.remove("timestamp");
    const auto answer = call("PUT", u"/syncs/progress"_s, body);
    if (!answer)
        return std::unexpected(answer.error());
    return {};
}
