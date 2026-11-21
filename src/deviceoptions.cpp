#include "deviceoptions.h"

#include <QAbstractSocket>
#include <QRandomGenerator>
#include <algorithm>
#include <array>
#include <utility>

#include "secretstore.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto reaches = std::to_array<std::pair<DeviceOptions::Reach, QLatin1StringView>>({
    {DeviceOptions::Reach::Off, "off"_L1},
    {DeviceOptions::Reach::ThisComputer, "localhost"_L1},
    {DeviceOptions::Reach::Tailnet, "tailnet"_L1},
    {DeviceOptions::Reach::Network, "network"_L1},
});

[[nodiscard]] quint16 portOf(const QJsonValue &value, quint16 otherwise) {
    // Typed into the window it comes as text.
    const int port = value.isString() ? value.toString().toInt() : value.toInt(otherwise);
    return port > 0 && port < 65536 ? static_cast<quint16>(port) : otherwise;
}

} // namespace

QString DeviceOptions::reachName(Reach reach) {
    const auto *const found = std::ranges::find(reaches, reach, &std::pair<Reach, QLatin1StringView>::first);
    return found == reaches.end() ? u"off"_s : QString(found->second);
}

std::optional<DeviceOptions::Reach> DeviceOptions::reachNamed(const QString &name) {
    const auto *const found = std::ranges::find(reaches, name, &std::pair<Reach, QLatin1StringView>::second);
    if (found == reaches.end())
        return std::nullopt;
    return found->first;
}

DeviceOptions DeviceOptions::fromJson(const QJsonObject &object) {
    DeviceOptions options;
    options.reach = reachNamed(object.value("serve").toString()).value_or(Reach::Off);
    options.port = portOf(object.value("port"), options.port);
    options.opds = object.value("opds").toBool(true);
    options.collection = object.value("opds_collection").toString();
    options.koreaderServe = object.value("koreader_serve").toBool(false);
    options.koreaderServeUser = object.value("koreader_serve_user").toString();
    options.syncServer = object.value("koreader_server").toString();
    options.syncUser = object.value("koreader_user").toString();
    options.deviceFolder = object.value("device_folder").toString();
    options.kindleAddress = object.value("kindle_address").toString();
    options.mailHost = object.value("mail_host").toString();
    options.mailPort = portOf(object.value("mail_port"), options.mailPort);
    options.mailUser = object.value("mail_user").toString();
    options.mailFrom = object.value("mail_from").toString();
    options.mailStartTls = object.value("mail_security").toString("starttls") != "tls";
    return options;
}

QJsonObject DeviceOptions::toJson() const {
    return {{"serve", reachName(reach)},
            {"port", port},
            {"opds", opds},
            {"opds_collection", collection},
            {"koreader_serve", koreaderServe},
            {"koreader_serve_user", koreaderServeUser},
            {"koreader_server", syncServer},
            {"koreader_user", syncUser},
            {"device_folder", deviceFolder},
            {"kindle_address", kindleAddress},
            {"mail_host", mailHost},
            {"mail_port", mailPort},
            {"mail_user", mailUser},
            {"mail_from", mailFrom},
            {"mail_security", mailStartTls ? "starttls" : "tls"}};
}

bool DeviceReach::onTailnet(const QHostAddress &address) {
    return address.isInSubnet(QHostAddress(u"100.64.0.0"_s), 10) || address.isInSubnet(QHostAddress(u"fd7a:115c:a1e0::"_s), 48);
}

std::optional<QHostAddress> DeviceReach::address(DeviceOptions::Reach reach, const QList<QNetworkInterface> &interfaces) {
    switch (reach) {
    case DeviceOptions::Reach::Off:
        return std::nullopt;
    case DeviceOptions::Reach::ThisComputer:
        return QHostAddress(QHostAddress::LocalHost);
    case DeviceOptions::Reach::Network:
        return QHostAddress(QHostAddress::Any);
    case DeviceOptions::Reach::Tailnet:
        break;
    }
    // The tailnet's IPv4 address, which every device on it can reach.
    for (const QNetworkInterface &interface : interfaces) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp))
            continue;
        for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && onTailnet(entry.ip()))
                return entry.ip();
        }
    }
    return std::nullopt;
}

QString DeviceReach::shown(DeviceOptions::Reach reach, const QHostAddress &listening, quint16 port) {
    QString host = listening.toString();
    if (reach == DeviceOptions::Reach::Network) {
        // The first address another device on the same network can reach.
        const QList<QHostAddress> all = QNetworkInterface::allAddresses();
        const auto own = std::ranges::find_if(all, [](const QHostAddress &address) {
            return address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() && !onTailnet(address);
        });
        host = own == all.end() ? u"localhost"_s : own->toString();
    }
    return u"http://%1:%2"_s.arg(host).arg(port);
}

QString DeviceSecrets::newToken(SecretStore &secrets) {
    // Letters and digits that can't be taken for one another when typed into an e-reader.
    constexpr QLatin1StringView alphabet{"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789"};
    QString token;
    for (int i = 0; i < 24; ++i)
        token += alphabet.at(QRandomGenerator::system()->bounded(static_cast<int>(alphabet.size())));
    return secrets.put(QString::fromLatin1(opdsToken), token) ? token : QString();
}

QString DeviceSecrets::token(SecretStore &secrets) {
    const QString kept = secrets.get(QString::fromLatin1(opdsToken));
    return kept.isEmpty() ? newToken(secrets) : kept;
}
