#pragma once
// A stand-in for an e-reader or a phone app (#71): it asks Omabook's server
// over HTTP, as KOReader and OPDS apps do, and waits for the answer while the
// server, on the same thread, answers.

#include <QByteArray>
#include <QEventLoop>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <memory>

namespace FakeDevice {

struct Answer {
    int status = 0;
    QByteArray body;
    QByteArray type;
    QByteArray disposition;

    [[nodiscard]] QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

inline Answer ask(const QByteArray &verb, const QUrl &url, const QHash<QByteArray, QByteArray> &headers = {}, const QByteArray &body = {}) {
    QNetworkAccessManager network;
    QNetworkRequest request(url);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        request.setRawHeader(it.key(), it.value());
    const std::unique_ptr<QNetworkReply> reply(network.sendCustomRequest(request, verb, body));
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    return {.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
            .body = reply->readAll(),
            .type = reply->rawHeader("Content-Type"),
            .disposition = reply->rawHeader("Content-Disposition")};
}

// What KOReader's progress sync plugin sends with every request: its media
// type, and the person's name and key (the MD5 of their password).
inline QHash<QByteArray, QByteArray> koreader(const QByteArray &user, const QByteArray &key) {
    return {
        {"Accept", "application/vnd.koreader.v1+json"}, {"Content-Type", "application/json"}, {"x-auth-user", user}, {"x-auth-key", key}};
}

} // namespace FakeDevice
