#pragma once

#include <QJsonObject>
#include <QList>
#include <QNetworkInterface>
#include <QString>
#include <QTimer>
#include <expected>
#include <functional>
#include <memory>

#include "deviceoptions.h"
#include "failure.h"
#include "hubplace.h"

class DeviceServer;
class HubKeeper;
class HubRequests;

// Phones and e-readers served by the hub (#193, #177): the OPDS catalog and
// KOReader's progress sync of one of its libraries, by the server a computer
// serves them with (DeviceServer), always on with the daemon, so they reach
// the books with no computer awake. Every request is answered in the keeper's
// turn (ADR 0013), so a place KOReader sends is written by the hub's one
// writer, into its change log, and reaches the person's computers by the
// normal sync with the hub.
//
// It listens only where the reach chosen allows (DeviceReach), and nowhere
// until a computer chooses one; a reach it can't take yet (the tailnet before
// Tailscale is up) is tried again every minute. What was chosen is the hub's
// own: under "devices" in its settings (HubPlace::settings), the catalog's
// token and KOReader's key in its secrets (HubPlace::secret), never a keyring.
//
// Asked over the daemon's socket, and through a computer's session:
//   {"request": "devices"}: how it stands (status());
//   {"request": "devices-set", "options": {"serve": "off" | "localhost" | "tailnet" | "network",
//    "port", "opds", "opds_collection", "koreader_serve", "koreader_serve_user", "library"},
//    "password": KOReader's (empty forgets it), "new_token": true}: any of them changed, then how it
//    stands; a choice it doesn't know is refused, and nothing changes.
class HubDevices {
public:
    // What it finds around it: tailscale, for the hub's name on the
    // tailnet, and the network's interfaces; tests pass their own.
    struct Around {
        QString tailscale = QStringLiteral("tailscale");
        std::function<QList<QNetworkInterface>()> interfaces = [] { return QNetworkInterface::allInterfaces(); };
    };

    // How long it waits before trying a reach again that it couldn't take.
    static constexpr int again = 60 * 1000;

    HubDevices(HubKeeper &keeper, HubPlace place);
    HubDevices(HubKeeper &keeper, HubPlace place, Around around);
    ~HubDevices();
    HubDevices(const HubDevices &) = delete;
    HubDevices &operator=(const HubDevices &) = delete;
    HubDevices(HubDevices &&) = delete;
    HubDevices &operator=(HubDevices &&) = delete;

    // Serves as its settings say now, or stops; after the keeper opened the libraries.
    void follow();

    // {"options": what was chosen (as devices-set takes it, "library" the one served), "libraries": the
    // hub's, "address": where devices reach it ("http://100.101.102.103:6622"; empty while it serves
    // nothing), "named": the same by the hub's name on the tailnet, when it has one, "token": the
    // catalog's, "password": whether KOReader's is set, "devices": how many asked in the last day, and
    // when it can't serve as chosen, "problem" ("no-tailnet", "port-taken", "other") and "said" (why, in English)}.
    [[nodiscard]] QJsonObject status() const;
    // What devices-set does.
    [[nodiscard]] std::expected<QJsonObject, Failure> set(const QJsonObject &asked);
    // "devices" and "devices-set".
    void addTo(HubRequests &requests);

private:
    [[nodiscard]] QJsonObject chosen() const;
    [[nodiscard]] std::expected<void, Failure> keep(const QJsonObject &chosen) const;
    [[nodiscard]] std::expected<void, Failure> keepSecrets(const QJsonObject &asked);
    [[nodiscard]] QString secret(const char *name) const;
    [[nodiscard]] QString served() const;
    void lookUpName(DeviceOptions::Reach reach, const QString &address);

    HubKeeper &m_keeper;
    HubPlace m_place;
    Around m_around;
    std::unique_ptr<DeviceServer> m_server;
    QString m_library; // the keeper's name for the library served
    QString m_address, m_named;
    QString m_problem, m_said;
    QTimer m_again;
};
