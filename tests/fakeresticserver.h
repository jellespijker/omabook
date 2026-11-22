#pragma once
// A restic REST server on this computer (restic's rest: backend, the protocol
// of rest-server, v2), standing in for a destination away from home in
// snapshotdestinations_tests.cpp: rest-server isn't in the build image. It
// keeps what restic sends in a folder of the test's own, asks for a user and
// password (HTTP basic), and runs on a thread of its own, since the code under
// test waits for restic on the test's thread.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <atomic>
#include <memory>

class FakeResticServer {
public:
    // Keeps the repository in `folder`; asks for `user` and `password` unless they are empty.
    FakeResticServer(QString folder, QByteArray user, QByteArray password)
        : m_folder(std::move(folder)),
          m_authorization(user.isEmpty() ? QByteArray() : ("basic " + (user + ':' + password).toBase64()).toLower()) {
        m_context.moveToThread(&m_thread);
        m_server->moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(
            &m_context,
            [this] {
                m_server->listen(QHostAddress::LocalHost);
                m_port = m_server->serverPort();
                QObject::connect(m_server.get(), &QTcpServer::newConnection, m_server.get(),
                                 [this] { take(m_server->nextPendingConnection()); });
            },
            Qt::BlockingQueuedConnection);
    }
    ~FakeResticServer() {
        QMetaObject::invokeMethod(&m_context, [this] { m_server.reset(); }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    FakeResticServer(const FakeResticServer &) = delete;
    FakeResticServer &operator=(const FakeResticServer &) = delete;
    FakeResticServer(FakeResticServer &&) = delete;
    FakeResticServer &operator=(FakeResticServer &&) = delete;

    // restic's place for it, without the user or password.
    [[nodiscard]] QString location() const { return QStringLiteral("rest:http://127.0.0.1:%1/").arg(m_port.load()); }
    [[nodiscard]] int refused() const { return m_refused.load(); }
    [[nodiscard]] int answered() const { return m_answered.load(); }

private:
    struct Request {
        QByteArray method, path, query, head, body;
    };

    void take(QTcpSocket *socket) {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            *buffer += socket->readAll();
            const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QByteArray head = buffer->left(headerEnd).toLower();
            const qsizetype length = header(head, "content-length").toLongLong();
            if (buffer->size() < headerEnd + 4 + length)
                return;
            const QList<QByteArray> line = buffer->left(buffer->indexOf("\r\n")).split(' ');
            const QByteArray target = line.value(1);
            const qsizetype mark = target.indexOf('?');
            const Request request{.method = line.value(0),
                                  .path = mark < 0 ? target : target.left(mark),
                                  .query = mark < 0 ? QByteArray() : target.mid(mark + 1),
                                  .head = head,
                                  .body = buffer->mid(headerEnd + 4, length)};
            buffer->clear();
            socket->write(answer(request));
            socket->disconnectFromHost();
        });
    }

    [[nodiscard]] static QByteArray header(const QByteArray &head, const QByteArray &name) {
        const qsizetype at = head.indexOf("\r\n" + name + ':');
        if (at < 0)
            return {};
        const qsizetype from = at + name.size() + 3;
        const qsizetype end = head.indexOf("\r\n", from);
        return head.mid(from, end < 0 ? -1 : end - from).trimmed();
    }

    [[nodiscard]] static QByteArray reply(const QByteArray &status, const QByteArray &body = {}, const QByteArray &type = {},
                                          bool withBody = true) {
        QByteArray reply = "HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (!type.isEmpty())
            reply += "Content-Type: " + type + "\r\n";
        if (status.startsWith("401"))
            reply += "WWW-Authenticate: Basic realm=\"restic\"\r\n";
        return reply + "Connection: close\r\n\r\n" + (withBody ? body : QByteArray());
    }

    [[nodiscard]] QByteArray answer(const Request &request) {
        if (!m_authorization.isEmpty() && header(request.head, "authorization") != m_authorization) {
            ++m_refused;
            return reply("401 Unauthorized");
        }
        ++m_answered;
        const QString path = QString::fromUtf8(request.path).mid(1);
        if (request.method == "POST" && path.isEmpty() && request.query.contains("create=true")) {
            for (const char *type : {"data", "keys", "locks", "snapshots", "index"})
                QDir().mkpath(m_folder + '/' + QLatin1StringView(type));
            return reply("200 OK");
        }
        if (path.endsWith('/'))
            return list(path.chopped(1));
        const QString file = m_folder + '/' + path;
        if (request.method == "POST") {
            QFile out(file);
            const bool written = out.open(QIODevice::WriteOnly) && out.write(request.body) == request.body.size();
            return reply(written ? "200 OK" : "500 Internal Server Error");
        }
        if (request.method == "DELETE")
            return reply(QFile::remove(file) ? "200 OK" : "404 Not Found");
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly))
            return reply("404 Not Found");
        const QByteArray whole = in.readAll();
        if (request.method == "HEAD")
            return reply("200 OK", whole, {}, false);
        return ranged(whole, header(request.head, "range"));
    }

    // A part of the file when restic asks for one (Range: bytes=from-to), else all of it.
    [[nodiscard]] static QByteArray ranged(const QByteArray &whole, const QByteArray &range) {
        if (!range.startsWith("bytes="))
            return reply("200 OK", whole, "application/octet-stream");
        const QList<QByteArray> ends = range.mid(6).split('-');
        const qsizetype from = ends.value(0).toLongLong();
        const qsizetype to = ends.value(1).isEmpty() ? whole.size() - 1 : ends.value(1).toLongLong();
        return reply("206 Partial Content", whole.mid(from, to - from + 1), "application/octet-stream");
    }

    [[nodiscard]] QByteArray list(const QString &type) const {
        QJsonArray files;
        for (const QFileInfo &file : QDir(m_folder + '/' + type).entryInfoList(QDir::Files))
            files << QJsonObject{{"name", file.fileName()}, {"size", file.size()}};
        return reply("200 OK", QJsonDocument(files).toJson(QJsonDocument::Compact), "application/vnd.x.restic.rest.v2");
    }

    QString m_folder;
    QByteArray m_authorization; // lower case, as the headers are read
    QObject m_context;          // on the server's thread, where everything of the server's is done
    QThread m_thread;
    std::unique_ptr<QTcpServer> m_server = std::make_unique<QTcpServer>();
    std::atomic<quint16> m_port{0};
    std::atomic<int> m_refused{0}, m_answered{0};
};
