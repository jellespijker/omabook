#pragma once

#include <QFuture>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

#include "failure.h"
#include "hubautosync.h"
#include "hublinks.h"
#include "serveservice.h"
#include "sync.h"
#include "syncwait.h"

// The hubs this computer is linked to, as the window's Hubs panel shows them
// (#189, ADR 0012): each hub with how it stands (reachable, its Omabook, its
// libraries, the computers linked to it, its snapshots' warnings), linking a
// new one, unlinking, taking another computer's key off it, and syncing the
// library in use with it (#188), or seeing first what that would do. Everything is
// asked through the hub's session, with the requests the command line uses
// (`omabook hub status|add|remove|computers|forget|sync`); what ssh is asked runs
// on a worker, one thing at a time.
//
// And, where it is given what that needs (Automatic), automatic sync (#191,
// HubAutoSync): switched on per hub, then synced on the same worker when one
// is due and nothing else is being done, a refusal shown with its hub and
// told the desktop once; and the background service that does the same with
// the window closed, installed when the person allows it.
class HubDesk : public QObject {
    Q_OBJECT
    // Each linked hub, in the order linked: {"host", "computer" (this one's name there),
    // "fingerprint", "linked", "state" ("" before it was asked, "reachable", "unreachable",
    // "unlinked": the hub took the key away), "why" (when it isn't reachable), "version",
    // "libraries": [{"name", "books", "format"}], "computers": [{"name", "fingerprint", "linked", "this"}],
    // "snapshots" (whether they are set up there), "warnings": [the snapshots', each saying what to do],
    // "work": its background work, as the request "work" answers (HubWork::status, #192),
    // and with automatic sync: "auto" (switched on), "autoLast" (when its last sync began), "autoRefused" (what
    // it was refused with, and what to do), "autoUnreached" (its last try didn't reach the hub), "autoWaiting"
    // ("battery", "metered": why it waits now, SyncWait)}.
    Q_PROPERTY(QVariantList hubs READ hubs NOTIFY changed)
    // What the hub calls this computer unless told otherwise: its host name, as a word.
    Q_PROPERTY(QString thisComputer READ thisComputer CONSTANT)
    // Whether there is automatic sync here: the window's, not a test's that has none.
    Q_PROPERTY(bool automatic READ automatic CONSTANT)
    // Whether the background service syncs with the window closed.
    Q_PROPERTY(bool background READ background NOTIFY changed)
    // What is being done ("status", "link", "unlink", "forget", "sync", "preview", "cancel", "auto": an
    // automatic sync, "background"), and with which hub; empty when nothing is.
    Q_PROPERTY(QString doing READ doing NOTIFY changed)
    Q_PROPERTY(QString doingHost READ doingHost NOTIFY changed)
    // How the last of them went.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    // The last sync with a hub, or its preview: {"host", "dryRun", "lines": [what it did, or would do]}; empty before one.
    Q_PROPERTY(QVariantMap synced READ synced NOTIFY changed)
    // The last sync or preview, when the hub refused it for its library not being paired with
    // this computer's (#214): {"host", "there" (the hub's library), "here" (this computer's),
    // "dryRun"}, for pair() to pair them (#217); empty otherwise.
    Q_PROPERTY(QVariantMap unpaired READ unpaired NOTIFY changed)

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

    // What automatic sync needs: where it keeps how it stands (HubAutoSync), the clock, why to wait, how
    // the desktop is told, the background service's place, and how often it looks.
    struct Automatic {
        QString folder;
        std::function<QDateTime()> now = [] { return QDateTime::currentDateTimeUtc(); };
        std::function<SyncWait::Reason()> wait = SyncWait::now;
        std::function<void(const QString &title, const QString &body)> notify;
        ServeService::Where service = ServeService::usual();
        std::chrono::milliseconds every = std::chrono::seconds(30);
        HubAutoSync::Times times = {};
    };

    // This computer's links, kept in `folder`, signed by the person kept for the library at `root`.
    [[nodiscard]] static std::unique_ptr<Hubs> linkedHere(std::function<QString()> root, QString folder = HubLinks::usualFolder());

    explicit HubDesk(std::unique_ptr<Hubs> hubs, QObject *parent = nullptr);
    HubDesk(std::unique_ptr<Hubs> hubs, Automatic automatic, QObject *parent = nullptr);
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
    [[nodiscard]] QVariantMap unpaired() const { return m_unpaired; }
    [[nodiscard]] bool automatic() const noexcept { return m_auto != nullptr; }
    [[nodiscard]] bool background() const;

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
    // The sync, or preview, refused as `unpaired` says, asked again pairing the two
    // libraries, as `omabook hub sync <hub>:<library> --pair` does (#217). A preview
    // pairs nothing: it shows what the sync that pairs them would do.
    Q_INVOKABLE void pair();
    // Stops the background work under way on the hub at `host` (#192), keeping nothing of it.
    Q_INVOKABLE void cancelWork(const QString &host);
    // What was said is said no more.
    Q_INVOKABLE void clear();
    // Automatic sync with the hub at `host` switched on or off; on, it syncs at once when due.
    Q_INVOKABLE void setAutomatic(const QString &host, bool on);
    // The background service installed, to sync with the window closed too, or taken away.
    Q_INVOKABLE void setBackground(bool on);

    // Syncs with the hub that is due, if one is, nothing else is being done and nothing says to wait.
    void tick();
    // The library in use changed; the person did something in the window; the network is back.
    void libraryChanged();
    void touched();
    void cameIntoReach();
    // What the person does in the window (a key, a click, a wheel, a touch) is touched().
    bool eventFilter(QObject *watched, QEvent *event) override;

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
        QVariantMap synced;   // a sync's, when it was one
        QVariantMap unpaired; // a sync's, refused for the libraries not being paired
        std::optional<SyncSummary> automatic; // an automatic sync's
        std::optional<QDateTime> byHand;      // when a sync by hand that went through began
    };
    void sync(const QString &host, bool dryRun, bool pair);
    void work(const QString &doing, const QString &host, std::function<Outcome()> job);
    void finish(const QString &doing, const QString &host, const Outcome &outcome);
    // How the hub at `host` stands, asked on the worker.
    [[nodiscard]] static QVariantMap standing(const Hubs &hubs, const QString &host);

    std::unique_ptr<Hubs> m_hubs;
    QList<HubLinks::Hub> m_linked;
    QMap<QString, QVariantMap> m_states; // by host
    QFuture<void> m_job;
    QVariantMap m_synced, m_unpaired;
    QString m_doing, m_doingHost, m_status;
    bool m_failed = false;
    Automatic m_automatic;
    std::unique_ptr<HubAutoSync> m_auto;
    QMap<QString, SyncWait::Reason> m_waiting; // by host
    QTimer m_ticking;
    bool m_ownChange = false; // the library changes as a sync brought: no change to sync again
};
