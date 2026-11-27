#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <functional>
#include <optional>
#include <utility>

#include "hubcomputers.h"

// What a computer's key runs on its hub (#180, ADR 0012): `omabook hub
// session --key <fingerprint>`, the key's forced command. It speaks a small
// protocol, versioned, one JSON object a line each way, and takes nothing
// outside it:
//
//   computer: {"request": "hello", "protocol": 1}
//   hub:      {"ok": true, "protocol": 1, "computer": "laptop", "version": "0.5.0",
//              "requests": ["computers", "devices", "devices-set", "forget", "hello", "snapshots", "status", "sync",
//                           "unlink", "work", "work-cancel"]}
//
// The first line must be that hello, in the protocol the hub speaks; anything
// else ends the session with {"ok": false, "error": ..., "protocol": 1}. The
// computer is the one whose key the session runs for, as the hub wrote it in
// the key's line; a key that isn't linked any more, or a computer that isn't
// the hub owner's, is told so ("linked": false) and the session ends.
//
// Then each line is a request: `computers`, `unlink` (this computer's own
// key) and `forget` ({"name": another of the owner's computers}, its key taken
// away, as `omabook hub forget` does on the hub, #189) are answered here; the hub's own requests the session hands on
// (relayed(): `status`, `snapshots` and its warnings (#184), `sync` (HubSync, #188), and the hub's
// background work, `work` and `work-cancel` (HubWork, #192), and its phones and e-readers, `devices` and
// `devices-set` (HubDevices, #193)) go to the daemon (ADR 0013) with
// "computer" set to this computer's name, whatever the line said. `store`,
// another computer's or hub's snapshots kept here (HubStore, #196), is
// answered here too, beside the daemon, where the session was given a place
// for them. Any other request is refused. Answers are HubRequests' ({"ok": true, ...} or
// {"ok": false, "error": ...}). Every refusal, a computer that claims another's
// name, and each session's start leave a line in the hub's log.
class HubSession {
public:
    static constexpr int protocol = 1;

    // Hands a request on to the daemon: its answer, one line.
    using Relay = std::function<QByteArray(const QByteArray &request)>;

    // The snapshots it is sent are kept in `stores` (HubStore); none are, when it is empty.
    HubSession(HubComputers computers, QString fingerprint, Relay relay, QString stores = {})
        : m_computers(std::move(computers)), m_fingerprint(std::move(fingerprint)), m_relay(std::move(relay)), m_stores(std::move(stores)) {
    }

    // The answer to one line the computer sent, as one line (without its newline).
    [[nodiscard]] QByteArray answer(const QByteArray &line);
    // Whether the session has ended: nothing more is read.
    [[nodiscard]] bool over() const noexcept { return m_over; }

    // What a computer says first.
    [[nodiscard]] static QJsonObject hello();
    // The hub's requests a session hands on to the daemon.
    [[nodiscard]] static const QStringList &relayed();
    // Whether `command`, what the client asked ssh to run there
    // (SSH_ORIGINAL_COMMAND), asks for the session: nothing, or the session itself.
    [[nodiscard]] static bool asksForSession(QStringView command);
    // What a client sent, as the hub's log keeps it: one line, cut short.
    [[nodiscard]] static QString shown(QStringView said);

private:
    [[nodiscard]] QJsonObject greet(const QJsonObject &asked);
    [[nodiscard]] QJsonObject answerTo(const QJsonObject &asked);
    [[nodiscard]] QJsonObject ending(QJsonObject answer);
    [[nodiscard]] QJsonObject computers() const;
    // Takes the key of the computer `named` away, as `asking` asks.
    [[nodiscard]] QJsonObject forget(const QString &named, const HubComputers::Computer &asking);
    // Logs a request that names a computer other than the key's own.
    void noteOwner(const QJsonObject &asked, const HubComputers::Computer &computer);
    void noteClaim(const QJsonObject &asked, const QString &name) const;

    HubComputers m_computers;
    QString m_fingerprint;
    Relay m_relay;
    QString m_stores;
    std::optional<HubComputers::Computer> m_computer;
    bool m_over = false;
};
