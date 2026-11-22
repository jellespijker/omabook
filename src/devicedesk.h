#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QVariant>
#include <functional>

#include "deviceoptions.h"
#include "deviceserver.h"
#include "serveservice.h"

class SecretStore;
class Settings;

// Phones and e-readers as the window shows them (#71, #177): the server and
// where it listens, who serves (this window, the background service, or
// `omabook serve` in a terminal), KOReader's sync both ways, and sending a
// book to the mounted e-reader or a Kindle's address. The server, and what
// is asked of systemd, run on a thread of its own; syncing and sending on a
// worker, each with its own library.
class DeviceDesk : public QObject {
    Q_OBJECT
    // The options, as config.json keeps them under "devices" (DeviceOptions::toJson).
    Q_PROPERTY(QVariantMap options READ options NOTIFY changed)
    // Which secrets the keyring holds: {"serve", "sync", "mail"}, each true or false.
    Q_PROPERTY(QVariantMap secrets READ secrets NOTIFY changed)
    // Where devices reach the server, while it runs: "http://100.1.2.3:6622"; else empty.
    Q_PROPERTY(QString address READ address NOTIFY changed)
    // The same by this computer's name on the tailnet, when it has one there:
    // "http://laptop.example.ts.net:6622"; else empty.
    Q_PROPERTY(QString namedAddress READ namedAddress NOTIFY changed)
    // Who serves there: "window", "service" (the background service),
    // "command" (`omabook serve` in a terminal); empty while nobody does.
    Q_PROPERTY(QString servedBy READ servedBy NOTIFY changed)
    // The devices that asked for something in the last day.
    Q_PROPERTY(int devicesLately READ devicesLately NOTIFY changed)
    // The catalog's token, for an app that asks for it.
    Q_PROPERTY(QString token READ token NOTIFY changed)
    // Why the server isn't running when it should be; else empty.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // How the last sync or send went.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // The kind of e-reader at the device folder ("Kobo", "Kindle", "PocketBook",
    // "" for another), and whether one is there at all.
    Q_PROPERTY(QString deviceKind READ deviceKind NOTIFY changed)
    Q_PROPERTY(bool deviceThere READ deviceThere NOTIFY changed)
    Q_PROPERTY(bool canMail READ canMail NOTIFY changed)

public:
    using Root = std::function<QString()>;
    // The programs it runs: systemctl for the background service, and tailscale for this computer's name.
    struct Programs {
        ServeService::Where service = ServeService::usual();
        QString tailscale = QStringLiteral("tailscale");
    };

    DeviceDesk(Settings *settings, SecretStore &secrets, Root root, QObject *parent = nullptr);
    DeviceDesk(Settings *settings, SecretStore &secrets, Root root, Programs programs, QObject *parent = nullptr);
    ~DeviceDesk() override;
    DeviceDesk(const DeviceDesk &) = delete;
    DeviceDesk &operator=(const DeviceDesk &) = delete;
    DeviceDesk(DeviceDesk &&) = delete;
    DeviceDesk &operator=(DeviceDesk &&) = delete;

    [[nodiscard]] QVariantMap options() const;
    [[nodiscard]] QVariantMap secrets() const;
    [[nodiscard]] QString address() const { return m_address; }
    [[nodiscard]] QString namedAddress() const;
    [[nodiscard]] QString servedBy() const { return m_servedBy; }
    [[nodiscard]] int devicesLately() const noexcept { return m_devicesLately; }
    [[nodiscard]] QString token() const { return m_token; }
    [[nodiscard]] QString problem() const { return m_problem; }
    [[nodiscard]] QString status() const { return m_status; }
    [[nodiscard]] bool busy() const noexcept { return m_busy; }
    [[nodiscard]] QString deviceKind() const;
    [[nodiscard]] bool deviceThere() const;
    [[nodiscard]] bool canMail() const;

    // Changes one option by its key in config.json; the server follows.
    Q_INVOKABLE void set(const QString &key, const QVariant &value);
    // Keeps a password in the keyring, or forgets it when empty: "serve"
    // (what KOReader logs in to Omabook with), "sync" (Omabook's at a sync
    // server) or "mail" (the mail server's). Why it couldn't be kept; else empty.
    Q_INVOKABLE QString setPassword(const QString &which, const QString &password);
    // A new token for the catalog: every app then needs the new one.
    Q_INVOKABLE void renewToken();
    // Syncs every book's reading place with the KOReader sync server.
    Q_INVOKABLE void syncNow();
    // Sends a book to the mounted e-reader ("device") or by mail ("kindle").
    Q_INVOKABLE void sendBook(qint64 book, const QString &how);
    // Starts, restarts or stops the server as the options and the library are now.
    Q_INVOKABLE void follow();
    // Asks again who serves, and how many devices asked lately.
    Q_INVOKABLE void refresh();

signals:
    void changed();
    // A device moved a book's reading place, or a sync did, or a send kept a converted format.
    void booksChanged();

private:
    [[nodiscard]] DeviceOptions current() const;
    // Starts the server again when what it serves changed, or the secrets did.
    void restart(bool secretsChanged);
    void work(const std::function<QString()> &job);
    // The window's own server, started on its thread.
    void serve(const DeviceServer::Setup &setup, const QString &root);
    // The background service written and started, or started again; or stopped and gone.
    void followService(const DeviceOptions &options, bool wanted);
    void lookUpName();
    void told(const QString &servedBy, int devices, bool answered);

    Settings *m_settings;
    SecretStore &m_secrets;
    Root m_root;
    QThread m_thread;
    Programs m_programs;
    QPointer<QObject> m_jobs; // on the thread, for what is asked of systemd there in turn
    QPointer<DeviceServer> m_server;
    bool m_serviceFollowed = false; // the background service was seen to this session
    bool m_serviceUp = false;       // and systemd started it
    QJsonObject m_following; // what the server was last started with
    QString m_token;
    QString m_address;
    QString m_name; // this computer's on the tailnet
    QString m_servedBy;
    int m_devicesLately = 0;
    QString m_problem;
    QString m_status;
    bool m_busy = false;
};
