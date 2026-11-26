#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QThread>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

#include "failure.h"
#include "restic.h"
#include "snapshotdestinations.h"

class HttpServer;
struct HttpRequest;
struct HttpResponse;

// This computer's way to a repository one of its hubs keeps for it (#196, a
// destination "hub:<host>", HubStore there): restic's REST backend, served on
// this computer's loopback only, for as long as this lives, behind a user and
// a password made for it alone; each request restic makes is handed to the hub
// over this computer's session with it (HubLinks), as the request `store`, a
// piece at a time. restic never reaches a shell on the hub, nor anything there
// but that repository.
//
// It answers on a thread of its own, as restic is waited for on the one that
// runs it; the session is opened there, at restic's first request, so a
// destination only looked at reaches nothing.
class HubStoreBridge {
public:
    // Asks the hub one line, answered with one line; empty once the session ended.
    using Ask = std::function<QByteArray(const QByteArray &line)>;
    // A session with the hub, its hello said; asked on the bridge's thread.
    using Open = std::function<std::expected<Ask, Failure>()>;

    // The largest file restic sends in one request: its packs are 16 MiB unless told, at most 128.
    static constexpr qsizetype largest = qsizetype{160} * 1024 * 1024;

    // The repository `repository` the hub at `hub` keeps, reached by `open`.
    HubStoreBridge(QString hub, QString repository, Open open);
    ~HubStoreBridge();
    HubStoreBridge(const HubStoreBridge &) = delete;
    HubStoreBridge &operator=(const HubStoreBridge &) = delete;
    HubStoreBridge(HubStoreBridge &&) = delete;
    HubStoreBridge &operator=(HubStoreBridge &&) = delete;

    // Listens on the loopback.
    [[nodiscard]] std::expected<void, Failure> start();
    // The repository as restic reaches it here, without its password: kept going by `through`.
    [[nodiscard]] ResticRepository repository() const;

    // How SnapshotDestinations reaches a destination on a hub: through `open`'s session with the hub at a host.
    [[nodiscard]] static SnapshotDestinations::HubReach reach(std::function<std::expected<Ask, Failure>(const QString &host)> open);
    // The same through this computer's links to its hubs (HubLinks).
    [[nodiscard]] static SnapshotDestinations::HubReach linked();

private:
    [[nodiscard]] HttpResponse answer(const HttpRequest &request);
    [[nodiscard]] HttpResponse file(const HttpRequest &request, const QString &path);
    [[nodiscard]] HttpResponse read(const HttpRequest &request, const QString &path);
    [[nodiscard]] HttpResponse write(const QString &path, const QByteArray &body);
    // The hub's answer to `request` (its op and what it names), the repository's.
    [[nodiscard]] std::expected<QJsonObject, Failure> store(QJsonObject request);

    QString m_hub;
    QString m_repository;
    Open m_open;
    QByteArray m_user, m_password, m_authorization;
    std::optional<Ask> m_ask; // the bridge thread's
    std::optional<Failure> m_failed;
    QThread m_thread;
    std::unique_ptr<HttpServer> m_server;
    quint16 m_port = 0;
};
