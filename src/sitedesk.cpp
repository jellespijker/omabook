#include "sitedesk.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoopLocker>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QtConcurrent>
#include <algorithm>

#include "goodreads.h"
#include "library.h"
#include "settings.h"

namespace {

constexpr qint64 day = 24 * 60 * 60;
// After this long a site is worth bringing in again.
constexpr qint64 stale = 42 * day;

QJsonObject failure(const QString &scheme, const QString &action, const QString &error, const QString &problem = {}) {
    QJsonObject result{{"site", scheme}, {"action", action}, {"ok", false}, {"error", error}};
    if (!problem.isEmpty())
        result.insert("problem", problem);
    return result;
}

} // namespace

SiteDesk::SiteDesk(Settings *settings, QNetworkAccessManager *network, LibraryRoot root, QObject *parent)
    : QObject(parent), m_settings(settings), m_network(network), m_root(std::move(root)) {
    m_maker = [this](const QString &scheme, const QString &identity, const QString &secret, QObject *owner) {
        return ReadingSites::make(scheme, identity, secret, m_network, owner);
    };
    m_now = [] { return QDateTime::currentSecsSinceEpoch(); };
    connect(settings, &Settings::changed, this, &SiteDesk::changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { lookForExport(); });
}

SiteDesk::~SiteDesk() = default;

ReadingSite *SiteDesk::describe(const QString &scheme) const {
    if (!m_described.contains(scheme))
        m_described.insert(scheme, m_maker(scheme, {}, {}, &m_describing));
    return m_described.value(scheme);
}

QString SiteDesk::secretFor(const QString &scheme) const {
    // The keyring is only asked where something was put in it.
    if (!m_settings->site(scheme).value("secret").toBool())
        return {};
    return (m_secrets ? m_secrets : &Secrets::shared())->get(scheme);
}

QString SiteDesk::keyringNote() const {
    QString why;
    return (m_secrets ? m_secrets : &Secrets::shared())->lasting(&why) ? QString() : why;
}

QString SiteDesk::ago(qint64 since) const {
    const int days = int(qMax<qint64>(0, m_now() - since) / day);
    if (days == 0) return tr("today");
    if (days == 1) return tr("yesterday");
    if (days < 14) return tr("%Ln days ago", "", days);
    if (days < 70) return tr("%Ln weeks ago", "", days / 7);
    return tr("%Ln months ago", "", days / 30);
}

QString SiteDesk::nudge() const {
    for (const QString &scheme : ReadingSites::schemes()) {
        const qint64 synced = m_settings->site(scheme).value("synced").toInteger();
        if (ReadingSite *site = describe(scheme); site && synced > 0 && m_now() - synced > stale && m_settings->siteOn(scheme))
            return tr("%1 was last brought in %2. Bring it in again?").arg(site->name(), ago(synced));
    }
    return {};
}

QVariantList SiteDesk::rows() const {
    QVariantList rows;
    for (const QString &scheme : ReadingSites::schemes()) {
        ReadingSite *site = describe(scheme);
        if (!site)
            continue;
        const QJsonObject connection = m_settings->site(scheme);
        const QString identity = m_settings->siteIdentity(scheme);
        QString state = connection.value("state").toString();
        // Somebody named before connections were kept is connected, unlooked-at.
        if (state.isEmpty())
            state = identity.isEmpty() && !connection.value("secret").toBool() ? "none" : "connected";
        if (!m_settings->siteOn(scheme))
            state = "off";
        const QString name = connection.value("name").toString().isEmpty() ? identity : connection.value("name").toString();
        const int books = connection.value("books").toInt(-1), shelves = connection.value("shelves").toInt();
        const qint64 synced = connection.value("synced").toInteger();
        QStringList pending;
        for (const ReadingSites::Outgoing &change : m_pending.value(scheme).mid(0, 40))
            pending << change.title + ": " + change.note();
        rows << QVariantMap{
            {"scheme", scheme},
            {"name", site->name()},
            {"state", state},
            {"stateText", state == "connected" ? (name.isEmpty() ? tr("Connected") : tr("Connected as %1").arg(name))
                          : state == "attention" ? tr("Needs attention")
                          : state == "off" ? tr("Switched off") : tr("Not connected")},
            {"why", state == "attention" ? connection.value("why").toString()
                    : state == "off" ? tr("Its plugin is off, so nothing is asked of it.") : QString()},
            {"problem", state == "attention" ? connection.value("problem").toString() : QString()},
            {"identity", identity},
            {"detail", books < 0 ? QString()
                       : shelves > 0 ? tr("%1 on %2").arg(tr("%Ln books", "", books), tr("%Ln shelves", "", shelves))
                                     : tr("%Ln books", "", books)},
            {"syncedText", synced > 0 ? tr("Last brought in %1").arg(ago(synced)) : QString()},
            {"stale", synced > 0 && m_now() - synced > stale},
            {"secretUse", int(site->secretUse())},
            // Open Library's keys come as a pair.
            {"secretParts", scheme == "openlibrary" ? 2 : 1},
            {"hasSecret", connection.value("secret").toBool()},
            {"canWrite", scheme != "goodreads" && connection.value("secret").toBool()},
            {"write", connection.value("write").toBool()},
            {"pending", pending},
            {"pendingCount", m_pending.value(scheme).size()},
            {"busy", m_busy.contains(scheme)},
            {"status", m_busy.contains(scheme) ? m_busy.value(scheme) : m_status.value(scheme)},
            {"usesFile", scheme == "goodreads"},
        };
    }
    return rows;
}

QJsonArray SiteDesk::list() const {
    QJsonArray list;
    for (const QVariant &value : rows()) {
        const QVariantMap row = value.toMap();
        const QJsonObject connection = m_settings->site(row.value("scheme").toString());
        QJsonObject object{
            {"site", row.value("scheme").toString()},
            {"name", row.value("name").toString()},
            {"state", row.value("state").toString()},
            {"identity", row.value("identity").toString()},
            {"connected_as", connection.value("name").toString()},
            {"needs_token", row.value("secretUse").toInt() == 2},
            {"has_token", row.value("hasSecret").toBool()},
            {"writes_back", row.value("write").toBool()},
            {"stale", row.value("stale").toBool()},
        };
        if (!row.value("why").toString().isEmpty()) object.insert("why", row.value("why").toString());
        if (!row.value("problem").toString().isEmpty()) object.insert("problem", row.value("problem").toString());
        if (connection.contains("books")) object.insert("books", connection.value("books").toInt());
        if (connection.contains("shelves")) object.insert("shelves", connection.value("shelves").toInt());
        if (const qint64 synced = connection.value("synced").toInteger(); synced > 0)
            object.insert("last_sync", QDateTime::fromSecsSinceEpoch(synced).toString(Qt::ISODate));
        list << object;
    }
    return list;
}

QString SiteDesk::identityIn(const QString &scheme, const QString &pasted) const {
    ReadingSite *site = describe(scheme);
    return site ? site->identityFrom(pasted) : QString();
}

ReadingSite *SiteDesk::start(const QString &scheme, const QString &action) {
    if (m_busy.contains(scheme) || !describe(scheme))
        return nullptr;
    // What the last thing left behind (a site kept for a send) goes first.
    if (auto old = m_running.take(scheme); old.holder)
        old.holder->deleteLater();
    m_pending.remove(scheme);
    Running running;
    running.holder = new QObject(this);
    running.site = m_maker(scheme, m_settings->siteIdentity(scheme), secretFor(scheme), running.holder);
    ReadingSite *site = running.site;
    m_running.insert(scheme, running);
    m_busy.insert(scheme, action == "check" ? tr("Looking at %1…").arg(site->name()) : tr("Reading your shelves on %1…").arg(site->name()));
    emit changed();
    return site;
}

void SiteDesk::finish(const QString &scheme, QJsonObject result) {
    result.insert("site", scheme);
    m_busy.remove(scheme);
    m_results.insert(scheme, result);
    // A site whose changes wait to be looked over is kept, with what it
    // learned of the books; every other is done with. It may be the one
    // that is telling us so, and goes once it has finished speaking.
    if (!m_pending.contains(scheme))
        if (auto running = m_running.take(scheme); running.holder)
            running.holder->deleteLater();

    const QString name = describe(scheme) ? describe(scheme)->name() : scheme;
    QStringList lines;
    if (!result.value("ok").toBool()) {
        lines << result.value("error").toString();
    } else if (result.contains("new")) {
        lines << (result.value("dry_run").toBool() ? tr("Nothing was changed. A sync would bring: %1 new, %2 updated, %3 unchanged, %4 kept as they are here.")
                                                   : tr("%1 new, %2 updated, %3 unchanged, %4 kept as they are here."))
                     .arg(result.value("new").toInt()).arg(result.value("updated").toInt()).arg(result.value("unchanged").toInt())
                     .arg(result.value("kept").toInt());
        if (const int sent = result.value("sent").toInt(); sent > 0)
            lines << tr("%Ln changes sent to %1.", "", sent).arg(name);
        if (const int refused = result.value("send_failed").toArray().size(); refused > 0)
            lines << tr("%Ln changes were not taken by %1.", "", refused).arg(name);
        if (const int held = result.value("held").toInt(); held > 0)
            lines << tr("%Ln changes would go to %1. Look them over, then send them.", "", held).arg(name);
        else if (result.value("dry_run").toBool() && !result.value("to_send").toArray().isEmpty())
            lines << tr("%Ln changes would go to %1.", "", result.value("to_send").toArray().size()).arg(name);
        for (const QJsonValue &problem : result.value("problems").toArray())
            if (lines.size() < 6)
                lines << problem.toString();
    }
    m_status.insert(scheme, lines.join("\n"));
    emit changed();
    emit done(scheme, result);
}

void SiteDesk::failed(const QString &scheme, ReadingSite *site, const QString &message) {
    const SiteProblem problem = site->problem();
    QJsonObject connection = m_settings->site(scheme);
    // A site that can't be reached just now is no worse connected than it
    // was; one that says no is.
    const bool passing = problem == SiteProblem::Unreachable || problem == SiteProblem::Refused;
    if (!passing || connection.value("state").toString() != "connected") {
        connection.insert("state", "attention");
        connection.insert("problem", siteProblemWord(problem));
        connection.insert("why", message);
        connection.insert("checked", m_now());
        m_settings->setSite(scheme, connection);
    }
    finish(scheme, failure(scheme, m_results.value(scheme).value("action").toString("check"), message, siteProblemWord(problem)));
}

void SiteDesk::connectSite(const QString &scheme, const QString &pasted, const QString &secret) {
    ReadingSite *site = describe(scheme);
    if (!site || m_busy.contains(scheme))
        return;
    const QString identity = site->identityFrom(pasted);
    const QString given = !secret.trimmed().isEmpty() ? secret.trimmed() : site->secretIn(pasted);
    QJsonObject connection = m_settings->site(scheme);
    m_results.insert(scheme, QJsonObject{{"action", "connect"}});

    // What was pasted is of no use: said, and whatever connection there
    // was before is left as it is.
    auto refuse = [&](SiteProblem problem, const QString &why) {
        finish(scheme, failure(scheme, "connect", why, siteProblemWord(problem)));
    };
    if (identity.isEmpty() && !pasted.trimmed().isEmpty()) {
        refuse(SiteProblem::NoIdentity, QString("That names nobody on %1. Paste the address of your profile there, or your user name.").arg(site->name()));
        return;
    }
    if (!given.isEmpty()) {
        QString error;
        if (!(m_secrets ? m_secrets : &Secrets::shared())->put(scheme, given, &error)) {
            refuse(SiteProblem::Secret, error);
            return;
        }
        connection.insert("secret", true);
    }
    if (site->secretUse() == ReadingSite::NeedsSecret && !connection.value("secret").toBool()) {
        refuse(SiteProblem::Secret, QString("%1 needs a token. Make one on the page the button opens, and paste it here.").arg(site->name()));
        return;
    }
    if (identity.isEmpty() && !connection.value("secret").toBool()) {
        refuse(SiteProblem::NoIdentity, QString("Say who you are on %1: paste the address of your profile, or your user name.").arg(site->name()));
        return;
    }
    // A new person there is somebody else: what was known of the last one goes.
    if (identity != m_settings->siteIdentity(scheme)) {
        const bool kept = connection.value("secret").toBool();
        connection = QJsonObject();
        if (kept)
            connection.insert("secret", true);
    }
    m_settings->setSite(scheme, connection);
    m_settings->setSiteIdentity(scheme, identity);
    checkSite(scheme);
}

void SiteDesk::checkSite(const QString &scheme) {
    ReadingSite *site = start(scheme, "check");
    if (!site)
        return;
    m_results.insert(scheme, QJsonObject{{"action", "check"}});
    connect(site, &ReadingSite::checked, this, [this, scheme, site](const SiteProfile &profile) {
        QJsonObject connection = m_settings->site(scheme);
        if (!profile.identity.isEmpty() && profile.identity != m_settings->siteIdentity(scheme))
            m_settings->setSiteIdentity(scheme, profile.identity);
        connection = m_settings->site(scheme);
        connection.insert("name", profile.name);
        if (profile.books >= 0) connection.insert("books", profile.books);
        connection.insert("shelves", profile.shelves);
        connection.insert("checked", m_now());
        QJsonObject result{{"action", "check"}, {"ok", true}, {"name", profile.name}, {"books", profile.books}, {"shelves", profile.shelves},
                           {"shelves_readable", profile.shelvesReadable}};
        // Somebody found whose shelves can't be read is connected once
        // their shelves came in another way: the file.
        if (profile.shelvesReadable || connection.value("synced").toInteger() > 0) {
            connection.insert("state", "connected");
            connection.remove("problem");
            connection.remove("why");
        } else {
            const QString why = QString("%1 shows %2's profile%3 to anyone, but not the shelves. Bring them in with the file it lets you download.")
                                    .arg(site->name(), profile.name, profile.books >= 0 ? QString(" (%1 books)").arg(profile.books) : QString());
            connection.insert("state", "attention");
            connection.insert("problem", "file");
            connection.insert("why", why);
            result.insert("problem", "file");
            result.insert("why", why);
        }
        m_settings->setSite(scheme, connection);
        finish(scheme, result);
    });
    connect(site, &ReadingSite::failed, this, [this, scheme, site](const QString &message) { failed(scheme, site, message); });
    site->check();
}

void SiteDesk::disconnectSite(const QString &scheme) {
    if (!describe(scheme))
        return;
    m_busy.remove(scheme);
    if (auto running = m_running.take(scheme); running.holder)
        running.holder->deleteLater();
    m_pending.remove(scheme);
    m_status.remove(scheme);
    if (m_settings->site(scheme).value("secret").toBool())
        (m_secrets ? m_secrets : &Secrets::shared())->forget(scheme);
    m_settings->setSiteIdentity(scheme, {});
    m_settings->setSite(scheme, {});
    const QJsonObject result{{"site", scheme}, {"action", "disconnect"}, {"ok", true}};
    m_results.insert(scheme, result);
    emit changed();
    emit done(scheme, result);
}

void SiteDesk::setWrite(const QString &scheme, bool on) {
    QJsonObject connection = m_settings->site(scheme);
    if (connection.value("write").toBool() == on)
        return;
    connection.insert("write", on);
    if (!on) {
        m_pending.remove(scheme);
        // Turned off and on again, the first send is looked over again.
        connection.remove("sent");
    }
    m_settings->setSite(scheme, connection);
    emit changed();
}

void SiteDesk::sync(const QString &scheme, const SyncOptions &options) {
    if (m_root().isEmpty()) {
        finish(scheme, failure(scheme, "sync", "There is no library open."));
        return;
    }
    if (!m_settings->siteOn(scheme)) {
        finish(scheme, failure(scheme, "sync", QString("The %1 plugin is off, so nothing is asked of it.").arg(describe(scheme) ? describe(scheme)->name() : scheme)));
        return;
    }
    ReadingSite *site = start(scheme, "sync");
    if (!site)
        return;
    m_results.insert(scheme, QJsonObject{{"action", "sync"}});
    connect(site, &ReadingSite::fetched, this, [this, scheme, options](const QList<SiteEntry> &entries) {
        bringIn(scheme, entries, options, false);
    });
    connect(site, &ReadingSite::failed, this, [this, scheme, site](const QString &message) { failed(scheme, site, message); });
    site->fetchShelves();
}

// A shelf of a thousand books is a few thousand writes, so they happen on a
// worker with its own connection to the library, and the app stays running
// until they are done.
void SiteDesk::bringIn(const QString &scheme, const QList<SiteEntry> &entries, const SyncOptions &options, bool fromFile) {
    ReadingSite *site = m_running.value(scheme).site;
    const QString name = describe(scheme)->name();
    m_busy.insert(scheme, tr("Bringing in %Ln books from %1…", "", entries.size()).arg(name));
    emit changed();
    ReadingSites::Options how;
    how.addNew = options.addNew;
    how.dryRun = options.dryRun;
    how.writeBack = !fromFile && site && site->writes() && m_settings->site(scheme).value("write").toBool();
    const QString root = m_root();
    QPointer<SiteDesk> self(this);
    (void)QtConcurrent::run([self, root, entries, scheme, how, options, fromFile] {
        QEventLoopLocker locker;
        ReadingSites::Summary summary;
        Library library(root);
        QString error;
        if (library.open(&error))
            summary = ReadingSites::merge(library, entries, scheme, how);
        else
            summary.problems << error;
        QMetaObject::invokeMethod(qApp, [self, scheme, summary, options, fromFile] {
            if (self)
                self->merged(scheme, summary, options, fromFile);
        }, Qt::QueuedConnection);
    });
}

void SiteDesk::merged(const QString &scheme, const ReadingSites::Summary &summary, const SyncOptions &options, bool fromFile) {
    QJsonObject result = summary.toJson();
    result.insert("action", fromFile ? "import" : "sync");
    result.insert("ok", true);
    if (options.dryRun)
        result.insert("dry_run", true);
    QJsonObject connection = m_settings->site(scheme);
    if (!options.dryRun) {
        connection.insert("synced", m_now());
        connection.insert("state", "connected");
        connection.remove("problem");
        connection.remove("why");
        m_settings->setSite(scheme, connection);
        emit libraryChanged();
        emit coversWanted(summary.covers);
    }
    const bool writes = !fromFile && connection.value("write").toBool() && !summary.outgoing.isEmpty();
    if (!writes || options.dryRun) {
        if (!writes)
            result.remove("to_send");
        finish(scheme, result);
        return;
    }
    // The first time changes would go to a site they are shown, not sent.
    if (!connection.value("sent").toBool() && !options.send) {
        m_pending.insert(scheme, summary.outgoing);
        result.insert("held", summary.outgoing.size());
        finish(scheme, result);
        return;
    }
    write(scheme, summary.outgoing, result);
}

void SiteDesk::write(const QString &scheme, const QList<ReadingSites::Outgoing> &changes, QJsonObject result) {
    ReadingSite *site = m_running.value(scheme).site;
    if (!site) {
        result.insert("ok", false);
        result.insert("error", "The changes couldn't be sent; sync again.");
        finish(scheme, result);
        return;
    }
    m_pending.remove(scheme);
    m_busy.insert(scheme, tr("Sending %Ln changes to %1…", "", changes.size()).arg(site->name()));
    emit changed();
    auto *once = new QObject(site);
    connect(site, &ReadingSite::wrote, once, [this, scheme, changes, result, once](const QList<SiteWritten> &results) mutable {
        once->deleteLater();
        QList<ReadingSites::Outgoing> taken;
        QJsonArray refused;
        for (const SiteWritten &written : results) {
            const auto asked = std::ranges::find(std::as_const(changes), written.bookId, &ReadingSites::Outgoing::bookId);
            if (asked == changes.cend())
                continue;
            if (written.error.isEmpty()) {
                ReadingSites::Outgoing change = *asked;
                if (!written.siteId.isEmpty())
                    change.siteId = written.siteId;
                taken << change;
            } else {
                refused << QJsonObject{{"id", asked->bookId}, {"title", asked->title}, {"error", written.error}};
            }
        }
        // What the site took is what it now says, as far as the next sync
        // is concerned.
        if (Library library(m_root()); !taken.isEmpty() && library.open())
            ReadingSites::sent(library, scheme, taken);
        QJsonObject connection = m_settings->site(scheme);
        connection.insert("sent", true);
        m_settings->setSite(scheme, connection);
        result.insert("sent", taken.size());
        result.insert("send_failed", refused);
        result.remove("held");
        finish(scheme, result);
    });
    connect(site, &ReadingSite::failed, once, [this, scheme, site, once](const QString &message) {
        once->deleteLater();
        failed(scheme, site, message);
    });
    site->write(changes);
}

void SiteDesk::sendPending(const QString &scheme) {
    if (m_busy.contains(scheme))
        return;
    const QList<ReadingSites::Outgoing> changes = m_pending.value(scheme);
    // The site that worked them out still knows the books; without it the
    // sync is done again, this time to the end.
    if (changes.isEmpty() || !m_running.value(scheme).site) {
        SyncOptions options;
        options.send = true;
        sync(scheme, options);
        return;
    }
    QJsonObject result = m_results.value(scheme);
    result.insert("action", "send");
    write(scheme, changes, result);
}

void SiteDesk::importFile(const QString &path) {
    const QString scheme = "goodreads";
    if (m_busy.contains(scheme) || path.isEmpty())
        return;
    QFile source(path);
    if (m_root().isEmpty() || !source.open(QIODevice::ReadOnly)) {
        finish(scheme, failure(scheme, "import", m_root().isEmpty() ? QString("There is no library open.") : "The file can't be read: " + path));
        return;
    }
    const ReadingSite::Export read = Goodreads::parseCsv(source.readAll());
    if (!read.error.isEmpty()) {
        finish(scheme, failure(scheme, "import", read.error));
        return;
    }
    if (m_offered == path)
        dismissOffer();
    m_results.insert(scheme, QJsonObject{{"action", "import"}});
    bringIn(scheme, read.entries, {}, true);
}

QString SiteDesk::offeredFileName() const { return QFileInfo(m_offered).fileName(); }

void SiteDesk::dismissOffer() {
    if (m_offered.isEmpty())
        return;
    m_dismissed << m_offered;
    m_offered.clear();
    emit offeredFileChanged();
}

void SiteDesk::watchDownloads(bool on) {
    const QString folder = m_downloads.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : m_downloads;
    if (on == m_watching || folder.isEmpty())
        return;
    m_watching = on;
    if (on) {
        if (QFileInfo(folder).isDir())
            m_watcher.addPath(folder);
        lookForExport();
    } else {
        if (!m_watcher.directories().isEmpty())
            m_watcher.removePaths(m_watcher.directories());
        if (!m_offered.isEmpty()) {
            m_offered.clear();
            emit offeredFileChanged();
        }
    }
}

// The newest export in the Downloads folder, when it is whole and recent:
// one from last year is not the one somebody is fetching now.
void SiteDesk::lookForExport() {
    if (!m_watching)
        return;
    const QString folder = m_downloads.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : m_downloads;
    QString found;
    for (const QFileInfo &file : QDir(folder).entryInfoList({"goodreads_library_export*.csv"}, QDir::Files, QDir::Time)) {
        // A browser writes a download under another name first; the real
        // name may be there already, empty, with the other beside it.
        const bool whole = file.size() > 0 && !QFileInfo::exists(file.filePath() + ".part") && !QFileInfo::exists(file.filePath() + ".crdownload");
        if (whole && !m_dismissed.contains(file.filePath()) && file.lastModified().secsTo(QDateTime::currentDateTime()) < day) {
            found = file.filePath();
            break;
        }
    }
    if (found != m_offered) {
        m_offered = found;
        emit offeredFileChanged();
    }
}

void SiteDesk::note(const QString &scheme, const QString &text) {
    m_status.insert(scheme, text);
    emit changed();
}

void SiteDesk::openPage(const QString &scheme, const QString &what) {
    ReadingSite *site = describe(scheme);
    const QUrl url = !site ? QUrl()
                     : what == "find" ? site->findMePage()
                     : what == "secret" ? site->secretPage()
                     : what == "export" || what == "import" ? GoodreadsSite::exportPage() : QUrl();
    if (!url.isValid())
        return;
    m_opener ? m_opener(url) : void(QDesktopServices::openUrl(url));
}
