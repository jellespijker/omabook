#include "deviceserving.h"

#include <QJsonDocument>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <algorithm>
#include <array>
#include <utility>

#include "portholder.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto names = std::to_array<std::pair<DeviceServing::By, QLatin1StringView>>({
    {DeviceServing::By::Window, "window"_L1},
    {DeviceServing::By::Service, "service"_L1},
    {DeviceServing::By::Command, "command"_L1},
    {DeviceServing::By::Hub, "hub"_L1},
});

[[nodiscard]] QHostAddress plain(const QHostAddress &address) {
    // An IPv4 peer of a socket listening on every address comes as ::ffff:a.b.c.d.
    bool isFour = false;
    const quint32 four = address.toIPv4Address(&isFour);
    return isFour ? QHostAddress(four) : address;
}

// The body of an HTTP answer, when its status is 200.
[[nodiscard]] std::optional<QByteArray> body(const QByteArray &answer) {
    const qsizetype end = answer.indexOf("\r\n\r\n");
    if (end < 0 || !answer.startsWith("HTTP/1.1 200"))
        return std::nullopt;
    return answer.mid(end + 4);
}

} // namespace

QString DeviceServing::byName(By by) {
    const auto *const found = std::ranges::find(names, by, &std::pair<By, QLatin1StringView>::first);
    return found == names.end() ? u"window"_s : QString(found->second);
}

QJsonObject DeviceServing::Status::toJson() const {
    return {{"app", "omabook"}, {"by", byName(by)}, {"opds", opds}, {"koreader", koreader}, {"devices", devices}};
}

std::optional<DeviceServing::Status> DeviceServing::Status::fromJson(const QJsonObject &object) {
    if (object.value("app").toString() != u"omabook")
        return std::nullopt;
    const QString name = object.value("by").toString();
    const auto *const found = std::ranges::find(names, name, &std::pair<By, QLatin1StringView>::second);
    if (found == names.end())
        return std::nullopt;
    return Status{.by = found->first,
                  .opds = object.value("opds").toBool(),
                  .koreader = object.value("koreader").toBool(),
                  .devices = object.value("devices").toInt()};
}

bool DeviceServing::fromHere(const QHostAddress &peer) {
    const QHostAddress asked = plain(peer);
    if (asked.isLoopback())
        return true;
    const QList<QHostAddress> own = QNetworkInterface::allAddresses();
    return std::ranges::any_of(own, [&asked](const QHostAddress &address) { return plain(address) == asked; });
}

std::optional<DeviceServing::Status> DeviceServing::ask(const QHostAddress &address, quint16 port, std::chrono::milliseconds wait) {
    const bool everywhere = address == QHostAddress(QHostAddress::Any) || address == QHostAddress(QHostAddress::AnyIPv6)
                            || address == QHostAddress(QHostAddress::AnyIPv4);
    const QHostAddress where = everywhere ? QHostAddress(QHostAddress::LocalHost) : address;
    QTcpSocket socket;
    socket.connectToHost(where, port);
    const int waiting = static_cast<int>(wait.count());
    if (!socket.waitForConnected(waiting))
        return std::nullopt;
    socket.write("GET "_ba + path + " HTTP/1.1\r\nHost: "_ba + where.toString().toLatin1() + "\r\nConnection: close\r\n\r\n");
    QByteArray answer;
    while (socket.state() == QAbstractSocket::ConnectedState && socket.waitForReadyRead(waiting))
        answer += socket.readAll();
    answer += socket.readAll();
    const auto json = body(answer);
    return json ? Status::fromJson(QJsonDocument::fromJson(*json).object()) : std::nullopt;
}

Failure DeviceServing::taken(const QHostAddress &address, quint16 port, const QString &shown) {
    if (const auto serving = ask(address, port)) {
        switch (serving->by) {
        case By::Window:
            return {Failure::Code::ServedByWindow, {shown}};
        case By::Service:
            return {Failure::Code::ServedByService, {shown}};
        case By::Command:
            return {Failure::Code::ServedByCommand, {shown}};
        case By::Hub:
            return {Failure::Code::ServedByHub, {shown}};
        }
    }
    const QString where = address.toString();
    if (const QString program = PortHolder::program(port); !program.isEmpty())
        return {Failure::Code::PortTakenBy, {where, QString::number(port), program}};
    return {Failure::Code::PortTaken, {where, QString::number(port)}};
}
