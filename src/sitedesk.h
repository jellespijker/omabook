#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <functional>
#include <memory>

#include "readingsite.h"
#include "secretstore.h"

class Settings;

// The reading sites as somebody is connected to them: one model for every
// site, used by the window, the command line and the MCP tools alike. A
// site is not connected, connected as somebody, or needs attention (and
// says why); the actions are the same for all: connect, look, sync,
// disconnect.
//
// What is remembered of a connection is in the settings (Settings::site),
// and its token or key only in the keyring. Nothing here ever removes a
// book, and disconnecting only forgets who the person is on the site.
class SiteDesk : public QObject {
    Q_OBJECT
    // One row per site, each {scheme, name, state ("none", "connected",
    // "attention" or "off"), stateText, why, identity, detail, syncedText,
    // stale, secretUse (0 none, 1 optional, 2 needed), secretParts,
    // hasSecret, canWrite, write, pending (what would be sent, as lines),
    // busy, status, usesFile}.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    // True while any site is being looked at, read, brought in or written
    // to, for the window to show that work is under way.
    Q_PROPERTY(bool working READ working NOTIFY changed)
    // Why tokens won't outlast the session, when they won't; else empty.
    Q_PROPERTY(QString keyringNote READ keyringNote NOTIFY changed)
    // A site not brought in for six weeks, as a sentence; else empty.
    Q_PROPERTY(QString nudge READ nudge NOTIFY changed)
    // A Goodreads export that turned up in the Downloads folder while it
    // was watched: offered, never imported without a click.
    Q_PROPERTY(QString offeredFile READ offeredFile NOTIFY offeredFileChanged)
    Q_PROPERTY(QString offeredFileName READ offeredFileName NOTIFY offeredFileChanged)

public:
    using LibraryRoot = std::function<QString()>;
    using Maker = std::function<ReadingSite *(const QString &scheme, const QString &identity, const QString &secret, QObject *parent)>;

    SiteDesk(Settings *settings, QNetworkAccessManager *network, LibraryRoot root, QObject *parent = nullptr);
    ~SiteDesk() override;

    // Tests put their own keyring, sites, browser, Downloads folder and
    // clock in place of the real ones.
    void setSecrets(SecretStore *secrets) { m_secrets = secrets; }
    void setMaker(Maker maker) { m_maker = std::move(maker); }
    void setOpener(std::function<void(const QUrl &)> opener) { m_opener = std::move(opener); }
    void setDownloads(const QString &folder) { m_downloads = folder; }
    void setClock(std::function<qint64()> now) { m_now = std::move(now); }

    QVariantList rows() const;
    QString keyringNote() const;
    QString nudge() const;
    QString offeredFile() const { return m_offered; }
    QString offeredFileName() const;
    // Every site with its state, for the command line and for agents. No
    // token or key is in it, nor anything that would give one away.
    QJsonArray list() const;
    // How the last thing asked of a site went: {"site", "action", "ok",
    // "error", "problem", and for a sync the summary}.
    QJsonObject lastResult(const QString &scheme) const { return m_results.value(scheme); }
    bool busy(const QString &scheme) const { return m_busy.contains(scheme); }
    bool working() const noexcept { return !m_busy.isEmpty(); }

    // Who `pasted` is on the site, as far as it can be told without asking
    // the site; empty when it names nobody there.
    Q_INVOKABLE QString identityIn(const QString &scheme, const QString &pasted) const;
    // Takes whatever the person has (a link, a name, a number; a token or
    // key where the site wants one), keeps it, and looks at once.
    Q_INVOKABLE void connectSite(const QString &scheme, const QString &pasted, const QString &secret = {});
    // Looks at the account again.
    Q_INVOKABLE void checkSite(const QString &scheme);
    // Forgets who the person is there, and the token. The books stay.
    Q_INVOKABLE void disconnectSite(const QString &scheme);

    struct SyncOptions {
        bool addNew = true;
        bool dryRun = false; // say what would change here and there; change nothing
        bool send = false;   // agree to the first write, when changes go to the site
    };
    void sync(const QString &scheme, const SyncOptions &options);
    // Brings the site's shelves in and, where the person turned that on,
    // sends what changed here. The first such send is held back and shown.
    Q_INVOKABLE void syncSite(const QString &scheme) { sync(scheme, {}); }
    // Sends what the last sync held back.
    Q_INVOKABLE void sendPending(const QString &scheme);
    // Whether changes made here go to the site too. Off until turned on.
    Q_INVOKABLE void setWrite(const QString &scheme, bool on);

    // Brings in the file Goodreads lets its users download.
    Q_INVOKABLE void importFile(const QString &path);
    Q_INVOKABLE void importUrl(const QUrl &url) { importFile(url.toLocalFile()); }
    Q_INVOKABLE void dismissOffer();
    // Follows the Downloads folder for such a file while the guide is open.
    Q_INVOKABLE void watchDownloads(bool on);
    // Opens one of the site's pages in the browser: "find" (the person's
    // own profile), "secret" (where the token is made), "export" (where
    // Goodreads hands out the file) or "import" (where it takes one back).
    Q_INVOKABLE void openPage(const QString &scheme, const QString &what);
    // Says something under a site's row: how an export went, for one.
    void note(const QString &scheme, const QString &text);

signals:
    void changed();
    void offeredFileChanged();
    // Something asked of a site is done, well or badly.
    void done(const QString &scheme, const QJsonObject &result);
    // Books were added or changed.
    void libraryChanged();
    void coversWanted(const QList<QPair<qint64, QUrl>> &covers);

private:
    // A site at work, and the object everything of that work hangs off:
    // owned by the desk, and let go of as soon as the work is done.
    struct Running {
        QPointer<QObject> holder;
        QPointer<ReadingSite> site;
    };
    ReadingSite *start(const QString &scheme, const QString &action);
    ReadingSite *describe(const QString &scheme) const;
    QString secretFor(const QString &scheme) const;
    void finish(const QString &scheme, QJsonObject result);
    void failed(const QString &scheme, ReadingSite *site, const QString &message);
    void bringIn(const QString &scheme, const QList<SiteEntry> &entries, const SyncOptions &options, bool fromFile);
    void merged(const QString &scheme, const ReadingSites::Summary &summary, const SyncOptions &options, bool fromFile);
    void write(const QString &scheme, const QList<ReadingSites::Outgoing> &changes, QJsonObject result);
    void lookForExport();
    QString ago(qint64 since) const;

    Settings *m_settings;
    QNetworkAccessManager *m_network;
    LibraryRoot m_root;
    SecretStore *m_secrets = nullptr;
    Maker m_maker;
    std::function<void(const QUrl &)> m_opener;
    std::function<qint64()> m_now;
    QString m_downloads;

    // What is going on with each site just now.
    QHash<QString, Running> m_running;
    QHash<QString, QString> m_busy;   // scheme → what it is doing
    QHash<QString, QString> m_status; // scheme → how the last thing went
    QHash<QString, QJsonObject> m_results;
    QHash<QString, QList<ReadingSites::Outgoing>> m_pending;
    mutable QHash<QString, ReadingSite *> m_described;
    mutable QObject m_describing;

    QFileSystemWatcher m_watcher;
    bool m_watching = false;
    QString m_offered;
    QStringList m_dismissed;
};
