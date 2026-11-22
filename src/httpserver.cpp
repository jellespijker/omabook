#include "httpserver.h"

#include <QFile>
#include <QTcpSocket>
#include <QUrl>
#include <memory>
#include <optional>

using namespace Qt::StringLiterals;

namespace {

constexpr qsizetype headLimit = qsizetype{64} * 1024;
constexpr qsizetype bodyLimit = qsizetype{1024} * 1024;
constexpr qint64 piece = qint64{64} * 1024;

[[nodiscard]] QByteArray reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 402:
        return "Payment Required";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    default:
        return "Error";
    }
}

// What has come in on one connection so far.
struct Incoming {
    QByteArray buffer;
    bool answered = false;
};

// The request in `buffer`, once all of it is there; none while more is to come.
// `tooLarge` says it never will be one to answer.
[[nodiscard]] std::optional<HttpRequest> parse(const QByteArray &buffer, bool &tooLarge) {
    const qsizetype end = buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        tooLarge = buffer.size() > headLimit;
        return std::nullopt;
    }
    const QList<QByteArray> lines = buffer.left(end).split('\n');
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    HttpRequest request;
    request.method = first.value(0).toUpper();
    const QUrl target(QString::fromUtf8(first.value(1)));
    request.path = target.path(QUrl::FullyDecoded);
    request.query = QUrlQuery(target);
    for (const QByteArray &line : lines.mid(1)) {
        const qsizetype colon = line.indexOf(':');
        if (colon > 0)
            request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }
    const qsizetype length = request.header("content-length").toLongLong();
    if (length > bodyLimit) {
        tooLarge = true;
        return std::nullopt;
    }
    if (buffer.size() < end + 4 + length)
        return std::nullopt;
    request.body = buffer.mid(end + 4, length);
    return request;
}

} // namespace

HttpServer::HttpServer(Handler handler, QObject *parent) : QObject(parent), m_handler(std::move(handler)), m_server(this) {
    connect(&m_server, &QTcpServer::pendingConnectionAvailable, this, [this] {
        while (QTcpSocket *socket = m_server.nextPendingConnection())
            take(socket);
    });
}

std::expected<quint16, Failure> HttpServer::listen(const QHostAddress &address, quint16 port) {
    close();
    if (!m_server.listen(address, port)) {
        // Who has it is for the one who asked to find out (DeviceServing::holder).
        if (m_server.serverError() == QAbstractSocket::AddressInUseError)
            return std::unexpected(Failure(Failure::Code::PortTaken, {address.toString(), QString::number(port)}));
        return std::unexpected(Failure(Failure::Code::ServerUnstarted, {address.toString(), m_server.errorString()}));
    }
    return m_server.serverPort();
}

void HttpServer::close() { m_server.close(); }

void HttpServer::take(QTcpSocket *socket) {
    socket->setParent(this);
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    const auto incoming = std::make_shared<Incoming>();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, incoming] {
        if (incoming->answered)
            return;
        incoming->buffer += socket->readAll();
        bool tooLarge = false;
        const auto request = parse(incoming->buffer, tooLarge);
        if (!request && !tooLarge)
            return;
        incoming->answered = true;
        if (tooLarge) {
            socket->write("HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        HttpRequest asked = *request;
        asked.peer = socket->peerAddress();
        answer(socket, asked);
    });
}

void HttpServer::answer(QTcpSocket *socket, const HttpRequest &request) {
    HttpResponse response = m_handler(request);
    const auto file = std::make_shared<QFile>(response.file);
    if (!response.file.isEmpty() && !file->open(QIODevice::ReadOnly))
        response = {404, "text/plain; charset=utf-8", "Not found", {}, {}};
    const qint64 length = response.file.isEmpty() ? response.body.size() : file->size();
    QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' + reason(response.status) + "\r\n";
    head += "Content-Type: " + response.contentType + "\r\nContent-Length: " + QByteArray::number(length) + "\r\nConnection: close\r\n";
    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        head += it.key() + ": " + it.value() + "\r\n";
    socket->write(head + "\r\n");
    if (request.method == "HEAD" || response.file.isEmpty() || !file->isOpen()) {
        if (request.method != "HEAD")
            socket->write(response.body);
        socket->disconnectFromHost();
        return;
    }
    // The file a piece at a time, each when the last has gone out.
    const auto more = [socket, file] {
        if (socket->bytesToWrite() > piece)
            return;
        if (file->atEnd()) {
            socket->disconnectFromHost();
            return;
        }
        socket->write(file->read(piece));
    };
    connect(socket, &QTcpSocket::bytesWritten, socket, more);
    more();
}
