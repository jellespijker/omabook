#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QUrl>
#include <expected>
#include <optional>

#include "failure.h"
#include "koreaderbooks.h"

// A client of a KOReader progress sync server (#71): the protocol of
// koreader-sync-server, which KOReader's progress sync plugin speaks, and
// which Omabook serves too (KosyncRoutes). The person is known by a name
// and a key, the MD5 of their password, sent with every request. Every call
// waits for its answer, so none is for the window's thread.
class KosyncClient {
public:
    KosyncClient(QNetworkAccessManager &network, QUrl server, QString user, QString key);

    // The key KOReader sends for a password.
    [[nodiscard]] static QString keyOf(const QString &password);

    // Whether the server knows the person by that name and key.
    [[nodiscard]] std::expected<void, Failure> authorize();
    // What the server knows of a document's place; none when nothing.
    [[nodiscard]] std::expected<std::optional<KoreaderRecord>, Failure> progress(const QString &document);
    // Tells the server a place; the server stamps it with its own time.
    [[nodiscard]] std::expected<void, Failure> put(const KoreaderRecord &record);

private:
    [[nodiscard]] std::expected<QJsonObject, Failure> call(const QByteArray &verb, const QString &path, const QJsonObject &body = {});

    QNetworkAccessManager *m_network; // never null: what it was made with
    QUrl m_server;
    QString m_user;
    QString m_key;
};
