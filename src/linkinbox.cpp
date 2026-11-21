#include "linkinbox.h"

#include <QDir>
#include <QLocalSocket>
#include <QStandardPaths>
#include <chrono>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

// How long a hand-over waits for the window to answer: it is on this computer.
constexpr auto answering = std::chrono::milliseconds(1000);
// A link is a line; anything longer is no link.
constexpr qsizetype longestLink = 4096;

[[nodiscard]] bool answers(const QString &name) {
    QLocalSocket probe;
    probe.connectToServer(name);
    return probe.waitForConnected(static_cast<int>(answering.count()));
}

} // namespace

LinkInbox::LinkInbox(QString name, QObject *parent) : QObject(parent), m_name(std::move(name)) {
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(*socket); });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            read(*socket);
        }
    });
}

QString LinkInbox::sharedName() {
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return QDir(runtime.isEmpty() ? QDir::tempPath() : runtime).filePath(u"omabook-window"_s);
}

bool LinkInbox::handOver(const QString &link, const QString &name) {
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(static_cast<int>(answering.count())))
        return false;
    socket.write(link.trimmed().toUtf8() + '\n');
    const bool sent = socket.waitForBytesWritten(static_cast<int>(answering.count()));
    socket.disconnectFromServer();
    return sent;
}

bool LinkInbox::listen() {
    if (m_server.isListening() || m_server.listen(m_name))
        return true;
    // A socket left by a window that is gone is taken over; one that answers is another window's.
    if (answers(m_name))
        return false;
    QLocalServer::removeServer(m_name);
    return m_server.listen(m_name);
}

void LinkInbox::read(QLocalSocket &socket) {
    while (socket.canReadLine()) {
        const QString link = QString::fromUtf8(socket.readLine(longestLink)).trimmed();
        if (!link.isEmpty())
            emit linked(link);
    }
    if (socket.bytesAvailable() > longestLink)
        socket.abort();
}
