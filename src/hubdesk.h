#pragma once

#include <QFuture>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <expected>
#include <functional>
#include <memory>

#include "failure.h"
#include "hublinks.h"
#include "sync.h"

// The hubs this computer is linked to, as the window's Hubs panel shows them
// (#189, ADR 0012): each hub with how it stands (reachable, its Omabook, its
// libraries, the computers linked to it, its snapshots' warnings), linking a
// new one, unlinking, taking another computer's key off it, and syncing the
// library in use with it (#188), or seeing first what that would do. Everything is
// asked through the hub's session, with the requests the command line uses
// (`omabook hub status|add|remove|computers|forget|sync`); what ssh is asked runs
// on a worker, one thing at a time.
class HubDesk : public QObject {
    Q_OBJECT
    // Each linked hub, in the order linked: {"host", "computer" (this one's name there),
    // "fingerprint", "linked", "state" ("" before it was asked, "reachable", "unreachable",
    // "unlinked": the hub took the key away), "why" (when it isn't reachable), "version",
    // "libraries": [{"name", "books", "format"}], "computers": [{"name", "fingerprint", "linked", "this"}],
    // "snapshots" (whether they are set up there), "warnings": [the snapshots', each saying what to do]}.
    Q_PROPERTY(QVariantList hubs READ hubs NOTIFY changed)
    // What the hub calls this computer unless told otherwise: its host name, as a word.
    Q_PROPERTY(QString thisComputer READ thisComputer CONSTANT)
    // What is being done ("status", "link", "unlink", "forget", "sync", "preview"), and with which hub; empty when nothing is.
    Q_PROPERTY(QString doing READ doing NOTIFY changed)
    Q_PROPERTY(QString doingHost READ doingHost NOTIFY changed)
    // How the last of them went.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    // The last sync with a hub, or its preview: {"host", "dryRun", "lines": [what it did, or would do]}; empty before one.
    Q_PROPERTY(QVariantMap synced READ synced NOTIFY changed)

public:
    // The hubs as this computer reaches them: HubLinks, or a test's hub.
    class Hubs {
    public:
        Hubs() = default;
        virtual ~Hubs() = default;
        Hubs(const Hubs &) = delete;
        Hubs &operator=(const Hubs &) = delete;
        Hubs(Hubs &&) = delete;
        Hubs &operator=(Hubs &&) = delete;

        [[nodiscard]] virtual QList<HubLinks::Hub> linked() const = 0;
        // Links this computer as `computer`, over the person's own ssh, signed by them.
        [[nodiscard]] virtual std::expected<HubLinks::Hub, Failure> add(const QString &host, const QString &computer) = 0;
        [[nodiscard]] virtual std::expected<HubLinks::Hub, Failure> remove(const QString &host) = 0;
        [[nodiscard]] virtual std::expected<QJsonObject, Failure> ask(const QString &host, const QJsonObject &request) const = 0;
        // The library in use synced with the hub's, through its session (HubRemote).
        [[nodiscard]] virtual SyncSummary sync(const QString &host, const SyncOptions &options) = 0;
    };

    // This computer's links, kept in `folder`, signed by the person kept for the library at `root`.
    [[nodiscard]] static std::unique_ptr<Hubs> linkedHere(std::function<QString()> root, QString folder = HubLinks::usualFolder());

    explicit HubDesk(std::unique_ptr<Hubs> hubs, QObject *parent = nullptr);
    ~HubDesk() override;
    HubDesk(const HubDesk &) = delete;
    HubDesk &operator=(const HubDesk &) = delete;
    HubDesk(HubDesk &&) = delete;
    HubDesk &operator=(HubDesk &&) = delete;

    [[nodiscard]] QVariantList hubs() const;
    [[nodiscard]] static QString thisComputer();
    [[nodiscard]] QString doing() const { return m_doing; }
    [[nodiscard]] QString doingHost() const { return m_doingHost; }
    [[nodiscard]] QString status() const { return m_status; }
    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] QVariantMap synced() const { return m_synced; }

    // Each hub asked again how it stands.
    Q_INVOKABLE void refresh();
    // Links this computer to the hub at `host`, as `computer` (thisComputer when empty).
    Q_INVOKABLE void link(const QString &host, const QString &computer);
    // Takes this computer's key off the hub, and the hub off the list.
    Q_INVOKABLE void unlink(const QString &host);
    // Takes another computer's key off the hub at `host`.
    Q_INVOKABLE void forget(const QString &host, const QString &computer);
    // Syncs the library in use with the hub at `host`; with `dryRun`, changes
    // nothing on either side and says what a sync would do.
    Q_INVOKABLE void syncNow(const QString &host, bool dryRun);
    // What was said is said no more.
    Q_INVOKABLE void clear();

signals:
    void changed();
    // A sync brought books, or what is said of them, into the library in use.
    void booksChanged();

private:
    // How a job went, in words, and how each hub it asked stands.
    struct Outcome {
        QString said;
        bool failed = false;
        QMap<QString, QVariantMap> states;
        QVariantMap synced; // a sync's, when it was one
    };
    void work(const QString &doing, const QString &host, std::function<Outcome()> job);
    // How the hub at `host` stands, asked on the worker.
    [[nodiscard]] static QVariantMap standing(const Hubs &hubs, const QString &host);

    std::unique_ptr<Hubs> m_hubs;
    QList<HubLinks::Hub> m_linked;
    QMap<QString, QVariantMap> m_states; // by host
    QFuture<void> m_job;
    QVariantMap m_synced;
    QString m_doing, m_doingHost, m_status;
    bool m_failed = false;
};
