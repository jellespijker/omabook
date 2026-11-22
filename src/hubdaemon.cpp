#include "hubdaemon.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QObject>
#include <cerrno>
#include <utility>

#include "hubkeeper.h"
#include "hubrequests.h"
#include "log.h"

using namespace Qt::StringLiterals;

bool HubDaemon::answersAt(const QString &socket, std::chrono::milliseconds wait) {
    QLocalSocket probe;
    probe.connectToServer(socket);
    return probe.waitForConnected(static_cast<int>(wait.count()));
}

std::expected<void, Failure> HubDaemon::listen(const QString &socket) {
    if (answersAt(socket))
        return std::unexpected(Failure(Failure::Code::HubRunning, {socket}));
    // The folder is this user's alone, as the socket in it is.
    const QString folder = QFileInfo(socket).absolutePath();
    if (!QDir().mkpath(folder) || !QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return std::unexpected(Failure(Failure::Code::HubCantListen, {socket, qt_error_string(errno)}));
    // Nothing answers there: what is there is a killed daemon's.
    QLocalServer::removeServer(socket);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(socket))
        return std::unexpected(Failure(Failure::Code::HubCantListen, {socket, m_server.errorString()}));
    QObject::connect(&m_server, &QLocalServer::newConnection, &m_server, [this] { take(); });
    qCInfo(lcHub).noquote() << "Taking requests at" << socket;
    return {};
}

void HubDaemon::take() {
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] { serve(*socket); });
        QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        // What came with the connection, before it was connected to.
        if (socket->bytesAvailable() > 0)
            serve(*socket);
    }
}

void HubDaemon::serve(QLocalSocket &socket) {
    while (socket.canReadLine()) {
        const QByteArray line = socket.readLine().trimmed();
        if (line.isEmpty())
            continue;
        socket.write(m_requests.answerLine(line, m_keeper));
    }
    if (std::cmp_greater(socket.bytesAvailable(), longestLine)) {
        qCWarning(lcHub) << "A request longer than" << longestLine << "bytes was turned away.";
        socket.write(QJsonDocument(HubRequests::refusal(Failure(Failure::Code::HubBadRequest))).toJson(QJsonDocument::Compact) + '\n');
        socket.disconnectFromServer();
    }
    socket.flush();
}
