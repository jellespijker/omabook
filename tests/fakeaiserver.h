#pragma once
// A server of the OpenAI-compatible API on this computer, standing in for an
// AI server (recognition_tests.cpp, handwrittennotes_tests.cpp).

#include <QByteArray>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <functional>
#include <memory>

// A server of the OpenAI-compatible API on this computer: it answers each
// request with the next reply in its queue, and keeps what it was sent.
class FakeServer : public QTcpServer {
public:
    struct Reply {
        QByteArray status = "200 OK";
        QByteArray body;
    };
    struct Request {
        QByteArray line; // "POST /v1/chat/completions HTTP/1.1"
        QByteArray body;
        [[nodiscard]] QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    };
    QList<Reply> replies;
    QList<Request> received;
    // Answers each request by what it asks, in place of the queue, when set.
    std::function<Reply(const Request &)> answering;

    explicit FakeServer(quint16 port = 0) {
        listen(QHostAddress::LocalHost, port);
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                *buffer += socket->readAll();
                const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                const QByteArray lower = buffer->left(headerEnd).toLower();
                const qsizetype at = lower.indexOf("content-length:");
                const qsizetype length = at < 0 ? 0 : lower.mid(at + 15, lower.indexOf("\r\n", at) - at - 15).trimmed().toLongLong();
                if (buffer->size() < headerEnd + 4 + length)
                    return;
                received.append({.line = buffer->left(buffer->indexOf("\r\n")), .body = buffer->mid(headerEnd + 4, length)});
                const Reply reply = answering           ? answering(received.last())
                                    : replies.isEmpty() ? Reply{.status = "500 Internal Server Error", .body = "{}"}
                                                        : replies.takeFirst();
                socket->write("HTTP/1.1 " + reply.status + "\r\nContent-Type: application/json\r\nContent-Length: "
                              + QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body);
                socket->disconnectFromHost();
            });
        });
    }

    [[nodiscard]] QString url() const { return QStringLiteral("http://127.0.0.1:%1/v1").arg(serverPort()); }
    void answer(const QJsonObject &json, const QByteArray &status = "200 OK") {
        replies.append({.status = status, .body = QJsonDocument(json).toJson(QJsonDocument::Compact)});
    }
    void read(const QString &text) {
        answer(
            {{"model", "llava"}, {"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"role", "assistant"}, {"content", text}}}}}}});
    }
};
