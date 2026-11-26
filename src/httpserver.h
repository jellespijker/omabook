#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QUrlQuery>
#include <expected>
#include <functional>

#include "failure.h"

// A request as the server read it: the method, the path (percent-decoded)
// and its query, the headers by their names in lower case, and the body.
struct HttpRequest {
    QByteArray method;
    QString path;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
    QHostAddress peer;

    [[nodiscard]] QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
};

// What is answered: a status, the type of what is sent and either a body or
// a file, which is sent from disk a piece at a time so a big book is never
// read into memory whole. A Content-Length among the headers is said in place
// of the body's: what a GET would send, answering a HEAD.
struct HttpResponse {
    int status = 200;
    QByteArray contentType = "text/plain; charset=utf-8";
    QByteArray body;
    QString file;
    QHash<QByteArray, QByteArray> headers;

    [[nodiscard]] static HttpResponse json(int status, const QByteArray &body) { return {status, "application/json", body, {}, {}}; }
};

// The little web server e-readers and phones talk to (#71): HTTP/1.1, one
// request a connection, on the address it is given and nowhere else. It
// knows nothing of books; whoever starts it hands it what to answer. A
// request too large to be one an e-reader sends (more than 64 KiB of
// headers, a body over 1 MiB) is turned away unread.
class HttpServer : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    explicit HttpServer(Handler handler, QObject *parent = nullptr);

    // A body larger than `bytes` is turned away unread (1 MiB unless told).
    void takeBodiesUpTo(qsizetype bytes) { m_bodyLimit = bytes; }

    // Listens on `address` and `port` (0 for any free one); the port it got.
    [[nodiscard]] std::expected<quint16, Failure> listen(const QHostAddress &address, quint16 port);
    void close();
    [[nodiscard]] bool listening() const { return m_server.isListening(); }

private:
    void take(QTcpSocket *socket);
    void answer(QTcpSocket *socket, const HttpRequest &request);

    Handler m_handler;
    qsizetype m_bodyLimit = qsizetype{1024} * 1024;
    // Its child, so that it goes along when the server is moved to a thread of its own.
    QTcpServer m_server;
};
