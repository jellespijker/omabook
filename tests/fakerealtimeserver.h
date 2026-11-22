#pragma once
// OpenAI's Realtime API on this computer (realtimevoice_tests.cpp, #169): a
// WebSocket server speaking the shapes of OpenAI's Realtime guide and
// reference (checked on 2026-11-22). Each connection is opened with
// session.created; each response.create is answered by what `answering`
// says, a response of sound by default; everything it is sent is kept.

#include <QByteArray>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkRequest>
#include <QPointer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <functional>
#include <utility>

class FakeRealtimeServer : public QWebSocketServer {
public:
    struct Connection {
        QNetworkRequest request; // the address (with its model) and the headers
        QList<QJsonObject> sent; // what the voice sent, in order
        QPointer<QWebSocket> socket;
        bool closed = false;
    };
    QList<Connection> connections;
    // What it answers a response.create with, in order.
    std::function<QList<QJsonObject>(const QJsonObject &)> answering = [](const QJsonObject &) { return sound(); };

    // Mono 16-bit samples at 24 kHz, as OpenAI sends them: 50 ms of a quiet tone.
    [[nodiscard]] static QByteArray pcm() {
        QByteArray samples;
        for (int at = 0; at < 1200; ++at) {
            const qint16 sample = at % 24 < 12 ? 800 : -800;
            samples.append(static_cast<char>(sample & 0xff));
            samples.append(static_cast<char>((sample >> 8) & 0xff));
        }
        return samples;
    }
    // A response that reads: its sound in two pieces, then done.
    [[nodiscard]] static QList<QJsonObject> sound() {
        const QByteArray whole = pcm();
        const QByteArray first = whole.left(whole.size() / 2);
        const QByteArray second = whole.mid(whole.size() / 2);
        return {QJsonObject{{"type", "response.created"}, {"response", QJsonObject{{"id", "resp_1"}, {"status", "in_progress"}}}},
                QJsonObject{
                    {"type", "response.output_audio.delta"}, {"response_id", "resp_1"}, {"delta", QString::fromLatin1(first.toBase64())}},
                QJsonObject{
                    {"type", "response.output_audio.delta"}, {"response_id", "resp_1"}, {"delta", QString::fromLatin1(second.toBase64())}},
                QJsonObject{{"type", "response.output_audio.done"}, {"response_id", "resp_1"}}, done("completed")};
    }
    [[nodiscard]] static QJsonObject done(const QString &status, const QJsonObject &details = {}) {
        QJsonObject response{{"id", "resp_1"}, {"status", status}};
        if (!details.isEmpty())
            response.insert("status_details", details);
        return {{"type", "response.done"}, {"response", response}};
    }
    [[nodiscard]] static QJsonObject error(const QString &type, const QString &code, const QString &message) {
        return {{"type", "error"}, {"error", QJsonObject{{"type", type}, {"code", code}, {"message", message}, {"param", QJsonValue()}}}};
    }

    FakeRealtimeServer() : QWebSocketServer(QStringLiteral("realtime"), QWebSocketServer::NonSecureMode) {
        listen(QHostAddress::LocalHost);
        connect(this, &QWebSocketServer::newConnection, this, [this] {
            QWebSocket *socket = nextPendingConnection();
            const qsizetype which = connections.size();
            connections.append({.request = socket->request(), .sent = {}, .socket = socket, .closed = false});
            connect(socket, &QWebSocket::disconnected, this, [this, which] { connections[which].closed = true; });
            connect(socket, &QWebSocket::textMessageReceived, this, [this, socket, which](const QString &message) {
                const QJsonObject event = QJsonDocument::fromJson(message.toUtf8()).object();
                connections[which].sent.append(event);
                if (event.value("type").toString() == "response.create")
                    for (const QJsonObject &answer : answering(event))
                        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
            });
            socket->sendTextMessage(
                QStringLiteral(R"({"type":"session.created","event_id":"event_1","session":{"id":"sess_1","type":"realtime"}})"));
        });
    }

    // Its sockets are its children, closed once what they report to is gone: they go quiet first.
    ~FakeRealtimeServer() override {
        for (const Connection &each : std::as_const(connections))
            if (each.socket)
                each.socket->disconnect(this);
    }
    FakeRealtimeServer(const FakeRealtimeServer &) = delete;
    FakeRealtimeServer &operator=(const FakeRealtimeServer &) = delete;
    FakeRealtimeServer(FakeRealtimeServer &&) = delete;
    FakeRealtimeServer &operator=(FakeRealtimeServer &&) = delete;

    [[nodiscard]] QString url() const { return QStringLiteral("ws://127.0.0.1:%1/v1/realtime").arg(serverPort()); }
    // What a connection sent of one type; nothing while it isn't there yet.
    [[nodiscard]] QList<QJsonObject> sentOf(qsizetype connection, const QString &type) const {
        QList<QJsonObject> found;
        if (connection >= connections.size())
            return found;
        for (const QJsonObject &event : connections.at(connection).sent)
            if (event.value("type").toString() == type)
                found.append(event);
        return found;
    }
};
