#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QNetworkInterface>
#include <QString>
#include <cstdint>
#include <optional>

class SecretStore;

// How Omabook meets one's e-readers and phones (#71), as config.json keeps it
// under "devices". Everything is off until the person turns it on, and the
// server listens only where they allow. No password, token or key is here:
// those are in the keyring (DeviceSecrets).
struct DeviceOptions {
    // Where the server listens: nowhere, on this computer only, on the
    // tailnet's address only, or on every network this computer is on.
    enum class Reach : std::uint8_t { Off, ThisComputer, Tailnet, Network };

    Reach reach = Reach::Off;
    quint16 port = 6622;
    // Served by a systemd user service of its own (ServeService), the window
    // closed or not, rather than by the window while it is open (#177).
    bool background = false;
    // What it serves: the OPDS catalog (of one collection, by its path, or of
    // the whole library when empty) and KOReader's progress sync, for one
    // person by the name they log in with.
    bool opds = true;
    QString collection;
    bool koreaderServe = false;
    QString koreaderServeUser;
    // A KOReader sync server elsewhere that Omabook syncs with as a client.
    QString syncServer;
    QString syncUser;
    // Sending a book to an e-reader: the folder it is mounted at, and the
    // address a Kindle takes books at, with the mail server that sends them.
    QString deviceFolder;
    QString kindleAddress;
    QString mailHost;
    quint16 mailPort = 587;
    QString mailUser;
    QString mailFrom;
    bool mailStartTls = true; // STARTTLS on the port; else TLS from the start (465)

    [[nodiscard]] static DeviceOptions fromJson(const QJsonObject &object);
    [[nodiscard]] QJsonObject toJson() const;

    [[nodiscard]] static QString reachName(Reach reach);
    [[nodiscard]] static std::optional<Reach> reachNamed(const QString &name);
};

namespace DeviceReach {

// The address to listen on for a reach: none when there is nowhere, as for
// the tailnet on a computer that isn't on one. `interfaces` are this
// computer's; tests pass their own.
[[nodiscard]] std::optional<QHostAddress> address(DeviceOptions::Reach reach,
                                                  const QList<QNetworkInterface> &interfaces = QNetworkInterface::allInterfaces());
// Whether an address is a tailnet's (Tailscale's 100.64.0.0/10, or its fd7a:115c:a1e0::/48).
[[nodiscard]] bool onTailnet(const QHostAddress &address);
// The address to tell a device, for the window to show: this computer's own
// on its network for a reach of every network.
[[nodiscard]] QString shown(DeviceOptions::Reach reach, const QHostAddress &listening, quint16 port);
// This computer's name on its tailnet ("laptop.example.ts.net"), from what
// `tailscale status --json` says; empty when it has none (no MagicDNS).
[[nodiscard]] QString tailnetName(const QByteArray &status);

} // namespace DeviceReach

// The secrets of all that, by their names in the keyring.
namespace DeviceSecrets {

inline constexpr auto opdsToken = "devices-opds";                  // what an app asks the catalog with
inline constexpr auto koreaderServeKey = "devices-koreader-serve"; // the key KOReader logs in to Omabook with
inline constexpr auto koreaderKey = "devices-koreader";            // the key Omabook logs in to a sync server with
inline constexpr auto mailPassword = "devices-mail";               // the mail server's password

// The catalog's token, made and kept the first time it is asked for. Empty
// when the keyring won't keep one.
[[nodiscard]] QString token(SecretStore &secrets);
// A new token in its place, which every app then needs.
[[nodiscard]] QString newToken(SecretStore &secrets);

} // namespace DeviceSecrets
