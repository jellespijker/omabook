#include "pluginsources.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include "mcp.h"
#include "scriptplugins.h"
#include "secretstore.h"
#include "settings.h"

// What a request came to.
struct PluginSources::Reply {
    int status = 0;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers; // by lower-case name
    QString error;
    bool offline = false;    // GitHub wasn't reached at all
    bool limited = false;    // GitHub asked to wait
    bool refused = false;    // it led somewhere that isn't GitHub, or was too large
    bool redirected = false; // the API sent the request on to another of its addresses
};

// What a source stands at: a release, the head of a branch, or a tag.
struct PluginSources::Target {
    QString kind, ref, commit;
    bool unchanged = false; // GitHub said nothing changed since the last look

    QJsonObject json() const { return {{"kind", kind}, {"ref", ref}, {"commit", commit}}; }
    static Target from(const QJsonObject &object) {
        return {object.value("kind").toString(), object.value("ref").toString(), object.value("commit").toString(), false};
    }
};

namespace {

QJsonObject problem(const QString &why) { return {{"error", why}}; }

bool failed(const QJsonObject &answer) { return ScriptTools::failed(answer); }

QString part(const QString &encoded) { return QUrl::fromPercentEncoding(encoded.toUtf8()); }

// A ref as part of a path: its slashes stay, everything odd is encoded.
QString inPath(const QString &ref) { return QString::fromUtf8(QUrl::toPercentEncoding(ref, "/")); }

// Whether what GitHub has differs from what is installed.
bool differs(const QJsonObject &installed, const QJsonObject &latest) {
    if (latest.isEmpty())
        return false;
    const QString have = installed.value("commit").toString(), there = latest.value("commit").toString();
    if (!have.isEmpty() && !there.isEmpty())
        return have != there;
    return installed.value("ref").toString() != latest.value("ref").toString();
}

// What a plugin on disk says of itself: its title, and what it may use.
struct Manifest {
    QString title;
    QStringList permissions;
    bool script = false;

    explicit Manifest(const QString &folder) {
        QFile file(folder + "/plugin.json");
        const QJsonObject object = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        title = object.value("name").toString(QFileInfo(folder).fileName());
        script = !object.value("script").toString().isEmpty();
        if (script)
            for (const QJsonValue &value : object.value("permissions").toArray())
                permissions << value.toString();
        else
            permissions << "program";
    }
};

QString clock(qint64 seconds) { return QDateTime::fromSecsSinceEpoch(seconds).toString("HH:mm"); }

} // namespace

bool PluginSources::parse(const QString &text, Address *address, QString *error) {
    static const char *const forms = "Give a GitHub repository: owner/repo, https://github.com/owner/repo, or a link to one of "
                                     "its releases, tags, branches or folders.";
    QString rest = text.trimmed().section('?', 0, 0).section('#', 0, 0);
    static const QRegularExpression scheme("^(https?|git|ssh)://(git@)?", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression scp("^git@github\\.com:", QRegularExpression::CaseInsensitiveOption);
    rest.remove(scp).remove(scheme);
    if (rest.startsWith("www.", Qt::CaseInsensitive))
        rest = rest.mid(4);
    if (rest.startsWith("github.com/", Qt::CaseInsensitive))
        rest = rest.mid(11);
    QStringList parts;
    for (const QString &piece : rest.split('/', Qt::SkipEmptyParts))
        parts << part(piece);
    static const QRegularExpression ownerName("^[A-Za-z0-9][A-Za-z0-9-]*$"), repoName("^[A-Za-z0-9._-]+$");
    if (parts.size() >= 2 && parts.at(1).endsWith(".git"))
        parts[1].chop(4);
    if (parts.size() < 2 || !ownerName.match(parts.at(0)).hasMatch() || !repoName.match(parts.at(1)).hasMatch()
        || parts.at(1) == "." || parts.at(1) == "..") {
        // A host with a dot in it where the owner would be is another site.
        *error = parts.size() >= 2 && parts.at(0).contains('.')
                     ? "Only GitHub repositories can be added, and " + parts.at(0) + " is somewhere else. " + forms
                     : "\"" + text.trimmed() + "\" doesn't name a repository. " + forms;
        return false;
    }
    Address found;
    found.owner = parts.at(0);
    found.repo = parts.at(1);
    const QStringList tail = parts.mid(2);
    const QString what = tail.value(0);
    QStringList folder;
    if (tail.isEmpty() || what == "releases") {
        if (tail.value(1) == "tag" && tail.size() >= 3)
            found.ref = tail.at(2);
    } else if (what == "tree" && tail.size() >= 2) {
        found.ref = tail.at(1);
        folder = tail.mid(2);
    } else if (what == "blob" && tail.size() >= 2) {
        found.ref = tail.at(1);
        folder = tail.mid(2);
        // A link to the plugin.json itself means the folder it is in.
        if (!folder.isEmpty())
            folder.removeLast();
    } else if (what == "commit" && tail.size() >= 2) {
        found.ref = tail.at(1);
    } else if (what == "archive" && tail.size() >= 4 && tail.at(1) == "refs") {
        // The address of a download: archive/refs/tags/v1.2.tar.gz
        found.ref = tail.mid(3).join('/');
        static const QRegularExpression packed("\\.(tar\\.gz|tgz|zip)$");
        found.ref.remove(packed);
    } else {
        *error = "\"" + text.trimmed() + "\" names something in " + found.repository() + " that isn't a release, tag, branch or folder. "
                 + forms;
        return false;
    }
    for (const QString &step : std::as_const(folder)) {
        if (step == ".." || step == "." || step.contains('\\')) {
            *error = "\"" + folder.join('/') + "\" isn't a folder inside the repository.";
            return false;
        }
    }
    found.subfolder = folder.join('/');
    *address = found;
    return true;
}

QString PluginSources::nameFor(const Address &address) {
    if (!address.subfolder.isEmpty())
        return PluginTools::folderName(address.subfolder.section('/', -1));
    static const QRegularExpression affix("^omabook-(plugin-)?|-omabook(-plugin)?$");
    QString name = address.repo.toLower();
    const QString bare = QString(name).remove(affix);
    return PluginTools::folderName(bare.isEmpty() ? name : bare);
}

PluginSources::Service PluginSources::Service::fromEnvironment() {
    Service service;
    // A stand-in, only where it is on this computer: what the tests of the
    // command line and the window use, and nothing an address can reach.
    const QUrl stand(qEnvironmentVariable("OMABOOK_GITHUB_API"));
    if (stand.isValid() && (stand.host() == "127.0.0.1" || stand.host() == "localhost")) {
        service.api = stand.toString(QUrl::StripTrailingSlash);
        service.hosts = QStringList{stand.host()};
        service.httpsOnly = false;
    }
    return service;
}

PluginSources::PluginSources(const PluginTools::Context &context, QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_folder(context.folder.isEmpty() ? ScriptPlugins::defaultFolder() : context.folder), m_settings(context.settings),
      m_network(network), m_service(Service::fromEnvironment()), m_clock([] { return QDateTime::currentSecsSinceEpoch(); }) {
    if (!m_settings) {
        m_ownSettings = std::make_unique<Settings>();
        m_settings = m_ownSettings.get();
    }
}

PluginSources::~PluginSources() = default;

// --- The registry ---

QJsonObject PluginSources::readAll() const {
    QFile file(m_folder + "/sources.json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool PluginSources::writeAll(const QJsonObject &all) const {
    QJsonObject whole = all;
    whole.insert("version", 1);
    QDir().mkpath(m_folder);
    QSaveFile file(m_folder + "/sources.json");
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(whole).toJson()) > 0 && file.commit();
}

void PluginSources::store(const QString &name, const QJsonObject &entry) const {
    QJsonObject all = readAll();
    QJsonObject sources = all.value("sources").toObject();
    sources.insert(name, entry);
    all.insert("sources", sources);
    writeAll(all);
}

QJsonObject PluginSources::source(const QString &name) const { return readAll().value("sources").toObject().value(name).toObject(); }

QJsonArray PluginSources::list() const {
    QJsonArray list;
    const QJsonObject sources = readAll().value("sources").toObject();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        QJsonObject entry = it.value().toObject();
        // How GitHub's answers are told apart is nobody's reading.
        entry.remove("etags");
        entry.insert("name", it.key());
        entry.insert("id", "folder:" + it.key());
        entry.insert("missing", !QFileInfo(m_folder + "/" + it.key()).isDir());
        entry.insert("update", entry.value("channel").toObject().value("kind").toString() != "tag"
                                   && differs(entry.value("installed").toObject(), entry.value("latest").toObject()));
        entry.insert("previous", entry.contains("previous") && QFileInfo(m_folder + "/.previous/" + it.key()).isDir());
        list << entry;
    }
    return list;
}

QJsonArray PluginSources::annotate(const QJsonArray &overview) const {
    QHash<QString, QJsonObject> byId;
    for (const QJsonValue &value : list())
        byId.insert(value.toObject().value("id").toString(), value.toObject());
    if (byId.isEmpty())
        return overview;
    QJsonArray annotated;
    for (const QJsonValue &value : overview) {
        QJsonObject row = value.toObject();
        const auto found = byId.constFind(row.value("id").toString());
        if (found != byId.constEnd())
            row.insert("source", *found);
        annotated << row;
    }
    return annotated;
}

QJsonArray PluginSources::issues() const {
    QJsonArray issues;
    for (const QJsonValue &value : list()) {
        const QJsonObject entry = value.toObject();
        if (entry.value("missing").toBool())
            continue;
        const QString name = entry.value("installed").toObject().value("title").toString(entry.value("name").toString());
        const QJsonObject pending = entry.value("pending").toObject();
        if (pending.value("reason").toString() == "omabook")
            issues << QJsonObject{{"id", entry.value("id")}, {"name", name}, {"issue", "update-needs-omabook"},
                                  {"needs", pending.value("needs")}};
        else if (!pending.isEmpty())
            issues << QJsonObject{{"id", entry.value("id")}, {"name", name}, {"issue", "review"},
                                  {"permissions", pending.value("permissions")}, {"reason", pending.value("reason")}};
        else if (!entry.value("problem").toString().isEmpty())
            issues << QJsonObject{{"id", entry.value("id")}, {"name", name}, {"issue", "source"}, {"problem", entry.value("problem")}};
        else if (entry.value("update").toBool())
            issues << QJsonObject{{"id", entry.value("id")}, {"name", name}, {"issue", "update"},
                                  {"version", entry.value("latest").toObject().value("ref")}};
    }
    return issues;
}

bool PluginSources::anyDue() const {
    for (const QJsonValue &value : list()) {
        const QJsonObject entry = value.toObject();
        if (!entry.value("missing").toBool() && entry.value("channel").toObject().value("kind").toString() != "tag"
            && now() - qint64(entry.value("checked_at").toDouble()) >= checkEvery)
            return true;
    }
    return false;
}

QJsonObject PluginSources::forget(const QString &name) {
    QJsonObject all = readAll();
    QJsonObject sources = all.value("sources").toObject();
    if (!sources.contains(name))
        return problem("No source is remembered for a plugin " + name + ".");
    const QJsonObject entry = sources.take(name).toObject();
    all.insert("sources", sources);
    writeAll(all);
    QDir(m_folder + "/.previous/" + name).removeRecursively();
    return {{"name", name}, {"repository", entry.value("repository")}, {"forgotten", true}};
}

QJsonObject PluginSources::follow(const QString &name) {
    QJsonObject entry = source(name);
    if (entry.isEmpty())
        return problem("No source is remembered for a plugin " + name + ".");
    entry.insert("channel", QJsonObject{{"kind", "auto"}});
    // What was known was known of the other channel.
    for (const char *key : {"etags", "latest", "pending", "problem", "skip"})
        entry.remove(key);
    entry.insert("checked_at", 0);
    store(name, entry);
    return {{"name", name}, {"channel", entry.value("channel")}};
}

QJsonObject PluginSources::rollBack(const QString &name) {
    QJsonObject entry = source(name);
    const QString target = m_folder + "/" + name, kept = m_folder + "/.previous/" + name;
    const QJsonObject previous = entry.value("previous").toObject();
    if (previous.isEmpty() || !QFileInfo(kept + "/plugin.json").isFile())
        return problem("There is no earlier version of " + name + " to go back to.");
    const QJsonObject left = entry.value("installed").toObject();
    // What the plugin kept of its own since the update stays with it.
    if (QFileInfo(target + "/data").isDir()) {
        QDir(kept + "/data").removeRecursively();
        QDir().rename(target + "/data", kept + "/data");
    }
    if (QFileInfo::exists(target) && !QFile::moveToTrash(target) && !QDir(target).removeRecursively())
        return problem("The version that is there, " + target + ", can't be moved out of the way.");
    QFile::remove(target + ".error");
    if (!QDir().rename(kept, target))
        return problem("The earlier version can't be put back in " + target + ".");
    entry.insert("installed", previous);
    entry.remove("previous");
    entry.remove("pending");
    // The one gone back from isn't taken again by itself; a newer one is.
    entry.insert("skip", left.value("commit"));
    store(name, entry);
    return {{"name", name}, {"id", "folder:" + name}, {"version", previous.value("version")}, {"from", left.value("version")},
            {"title", previous.value("title")}};
}

// --- Talking to GitHub ---

QString PluginSources::token(const QUrl &url) {
    if (!m_tokenRead) {
        m_tokenRead = true;
        // The keyring is only asked when a token was put there: asking it
        // can raise a prompt to unlock it, which a daily check must not do.
        if (m_settings->pluginToken())
            m_token = Secrets::shared().get("github");
    }
    // It opens the API and is no business of wherever a download comes from.
    return url.host() == QUrl(m_service.api).host() ? m_token : QString();
}

void PluginSources::get(const QUrl &url, const QHash<QByteArray, QByteArray> &headers, const QString &intoFile, int hops, bool redirected,
                        Answered done) {
    const bool secure = url.scheme() == "https" || (!m_service.httpsOnly && url.scheme() == "http");
    if (!secure || !m_service.hosts.contains(url.host(), Qt::CaseInsensitive)) {
        Reply refused;
        refused.refused = true;
        refused.error = "The request was sent on to " + (url.host().isEmpty() ? url.toString() : url.host())
                        + ", which isn't GitHub over HTTPS, so nothing was fetched.";
        done(refused);
        return;
    }
    QNetworkRequest request(url);
    // Redirects are followed here, one at a time, so each can be checked.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "Omabook/" OMABOOK_VERSION);
    for (auto it = headers.begin(); it != headers.end(); ++it)
        request.setRawHeader(it.key(), it.value());
    const QString secret = token(url);
    if (!secret.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + secret.toUtf8());

    // Made when first needed: most of what is asked of the sources asks nothing of GitHub.
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkReply *reply = m_network->get(request);
    reply->setParent(this);
    auto *file = intoFile.isEmpty() ? nullptr : new QFile(intoFile, reply);
    auto received = std::make_shared<qint64>(0);
    auto tooLarge = std::make_shared<bool>(false);
    const qint64 most = intoFile.isEmpty() ? 4 * 1024 * 1024 : m_limits.totalBytes;
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [=] {
        const QByteArray bytes = reply->readAll();
        *received += bytes.size();
        // Counted as it arrives: what a server says of its size isn't believed.
        if (*received > most) {
            *tooLarge = true;
            reply->abort();
            return;
        }
        if (!file) {
            body->append(bytes);
        } else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            if (file->isOpen() || file->open(QIODevice::WriteOnly))
                file->write(bytes);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [=] {
        Reply answer;
        answer.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        answer.redirected = redirected;
        for (const auto &header : reply->rawHeaderPairs())
            answer.headers.insert(header.first.toLower(), header.second);
        if (file)
            file->close();
        reply->deleteLater();
        const QByteArray location = answer.headers.value("location");
        if (QList<int>{301, 302, 303, 307, 308}.contains(answer.status) && !location.isEmpty()) {
            if (hops >= 5) {
                answer.refused = true;
                answer.error = "GitHub sent the request round in circles, so nothing was fetched.";
                done(answer);
                return;
            }
            const QUrl next = url.resolved(QUrl(QString::fromUtf8(location)));
            // The API sending a request on to another of its own addresses
            // is how a repository that moved or was renamed shows.
            const bool moved = redirected || (next.host() == url.host() && url.host() == QUrl(m_service.api).host());
            get(next, headers, intoFile, hops + 1, moved, done);
            return;
        }
        if (*tooLarge) {
            answer.refused = true;
            answer.error = QString("It is larger than %1 MB, which is more than a plugin is allowed, so it wasn't fetched.")
                               .arg(most / (1024 * 1024));
        } else if (answer.status == 0) {
            answer.offline = true;
            answer.error = "GitHub couldn't be reached: " + reply->errorString();
        }
        answer.body = *body;
        done(answer);
    });
}

void PluginSources::api(const Entry &entry, const QString &path, const QByteArray &accept, Answered done) {
    const qint64 until = qint64(readAll().value("wait_until").toDouble());
    if (until > now()) {
        Reply waiting;
        waiting.limited = true;
        waiting.error = "GitHub asked Omabook to wait until " + clock(until) + " before asking again (it allows about 60 "
                        "questions an hour), so nothing was asked.";
        done(waiting);
        return;
    }
    QHash<QByteArray, QByteArray> headers{{"Accept", accept}, {"X-GitHub-Api-Version", "2022-11-28"}};
    // What was answered last time is named, so "nothing changed" costs nothing.
    const QString etag = entry->value("etags").toObject().value(path).toString();
    if (!etag.isEmpty())
        headers.insert("If-None-Match", etag.toUtf8());
    get(QUrl(m_service.api + path), headers, {}, 0, false, [=](const Reply &reply) {
        Reply answer = reply;
        if (answer.status == 200 && answer.headers.contains("etag")) {
            QJsonObject etags = entry->value("etags").toObject();
            etags.insert(path, QString::fromUtf8(answer.headers.value("etag")));
            entry->insert("etags", etags);
        }
        if (answer.status == 403 || answer.status == 429) {
            qint64 wait = 0;
            if (answer.headers.value("x-ratelimit-remaining") == "0" && answer.headers.contains("x-ratelimit-reset"))
                wait = answer.headers.value("x-ratelimit-reset").toLongLong();
            else if (answer.headers.contains("retry-after"))
                wait = now() + answer.headers.value("retry-after").toLongLong();
            else if (answer.status == 429)
                wait = now() + 60;
            if (wait > 0) {
                // Remembered, so that neither this run nor the next asks before then.
                QJsonObject all = readAll();
                all.insert("wait_until", double(wait));
                writeAll(all);
                answer.limited = true;
                answer.error = "GitHub asked Omabook to wait until " + clock(wait) + " before asking again (it allows about 60 "
                               "questions an hour).";
            }
        }
        done(answer);
    });
}

void PluginSources::download(const QString &repository, const QString &ref, Answered done) {
    if (!m_downloads)
        m_downloads = std::make_unique<QTemporaryDir>();
    const QString file = m_downloads->filePath(QString("plugin-%1.tar.gz").arg(++m_downloadCount));
    get(QUrl(m_service.api + "/repos/" + repository + "/tarball/" + inPath(ref)),
        {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}}, file, 0, false, [=](const Reply &reply) {
            Reply answer = reply;
            // Where it was put, for the caller.
            answer.body = file.toUtf8();
            done(answer);
        });
}

QJsonObject PluginSources::failure(const Reply &reply, const QString &repository, bool installed) const {
    if (reply.limited)
        return {{"error", reply.error}, {"limited", true}};
    if (reply.offline)
        return {{"error", reply.error}, {"offline", true}};
    if (reply.refused)
        return {{"error", reply.error}, {"refused", true}};
    // Said of a source that is followed; one that is only being looked at has nothing installed.
    const QString untouched = installed ? QString(" The plugin that is installed is left as it is.") : QString();
    if (reply.redirected || reply.status == 301)
        return {{"error", repository + " has moved on GitHub." + untouched
                              + (installed ? " If you trust where it went, forget this source and add the plugin from its new address."
                                           : " Add it by its new address.")},
                {"gone", true}};
    if (reply.status == 404 || reply.status == 401 || reply.status == 403 || reply.status == 451)
        return {{"error", repository + (installed ? " isn't on GitHub any more, or isn't public." : " isn't on GitHub, or isn't public.")
                              + untouched},
                {"gone", true}};
    return problem(QString("GitHub answered %1 for %2.").arg(reply.status).arg(repository));
}

// --- What a source stands at ---

void PluginSources::commitOf(const Entry &entry, const QString &kind, const QString &ref, Resolved done) {
    const QString repository = entry->value("repository").toString();
    // Only the commit's id is asked for: forty letters, and an ETag with them.
    api(entry, "/repos/" + repository + "/commits/" + inPath(ref), "application/vnd.github.sha", [=](const Reply &reply) {
        Target target{kind, ref, {}, false};
        const QJsonObject seen = entry->value("latest").toObject();
        if (reply.status == 304 && seen.value("ref").toString() == ref) {
            target.commit = seen.value("commit").toString();
            target.unchanged = true;
        } else if (reply.status == 200 && !reply.redirected) {
            target.commit = QString::fromUtf8(reply.body).trimmed();
        } else if (!reply.limited && !reply.offline && !reply.refused && !reply.redirected && (reply.status == 404 || reply.status == 422)) {
            done({}, QJsonObject{{"error", "There is no branch, tag or commit \"" + ref + "\" in " + repository
                                              + " (any more). The plugin that is installed is left as it is."},
                                 {"gone", true}});
            return;
        } else {
            done({}, failure(reply, repository));
            return;
        }
        entry->insert("latest", target.json());
        done(target, {});
    });
}

void PluginSources::branchHead(const Entry &entry, bool again, Resolved done) {
    const QString repository = entry->value("repository").toString();
    const QString branch = entry->value("default_branch").toString();
    if (!branch.isEmpty()) {
        commitOf(entry, "branch", branch, [=](const Target &target, const QJsonObject &failure) {
            // A default branch that is gone was renamed: asked for once more.
            if (failure.value("gone").toBool() && !again && failure.value("error").toString().startsWith("There is no branch")) {
                entry->remove("default_branch");
                branchHead(entry, true, done);
                return;
            }
            done(target, failure);
        });
        return;
    }
    api(entry, "/repos/" + repository, "application/vnd.github+json", [=](const Reply &reply) {
        const QString named = QJsonDocument::fromJson(reply.body).object().value("default_branch").toString();
        if (reply.status != 200 || reply.redirected || named.isEmpty()) {
            done({}, failure(reply, repository));
            return;
        }
        entry->insert("default_branch", named);
        branchHead(entry, true, done);
    });
}

void PluginSources::resolve(const Entry &entry, Resolved done) {
    const QString repository = entry->value("repository").toString();
    const QJsonObject channel = entry->value("channel").toObject();
    const QString kind = channel.value("kind").toString("auto");
    if (kind == "branch" || kind == "tag") {
        commitOf(entry, kind, channel.value("name").toString(), done);
        return;
    }
    api(entry, "/repos/" + repository + "/releases/latest", "application/vnd.github+json", [=](const Reply &reply) {
        const QJsonObject seen = entry->value("latest").toObject();
        if (reply.status == 304 && seen.value("kind").toString() == "release") {
            Target target = Target::from(seen);
            target.unchanged = true;
            done(target, {});
        } else if (reply.status == 200 && !reply.redirected) {
            const QString tag = QJsonDocument::fromJson(reply.body).object().value("tag_name").toString();
            if (tag.isEmpty())
                branchHead(entry, false, done);
            else
                commitOf(entry, "release", tag, done);
        } else if (reply.status == 404 && !reply.redirected) {
            // No release: the default branch it is, if the repository is there.
            branchHead(entry, false, done);
        } else {
            done({}, failure(reply, repository));
        }
    });
}

void PluginSources::channelFor(const Entry &entry, const QString &ref, std::function<void(const QJsonObject &failure)> done) {
    const QString repository = entry->value("repository").toString();
    // Asked with nothing remembered: this is about what the ref is, not whether it changed.
    const Entry fresh = std::make_shared<QJsonObject>(QJsonObject{{"repository", repository}});
    api(fresh, "/repos/" + repository + "/branches/" + inPath(ref), "application/vnd.github+json", [=](const Reply &reply) {
        if (reply.status == 200 && !reply.redirected) {
            entry->insert("channel", QJsonObject{{"kind", "branch"}, {"name", ref}});
        } else if (reply.status == 404) {
            // Not a branch: a tag or a commit, which stays where it is.
            entry->insert("channel", QJsonObject{{"kind", "tag"}, {"name", ref}});
        } else {
            done(failure(reply, repository));
            return;
        }
        done({});
    });
}

// --- Adding ---

void PluginSources::fetch(const Entry &entry, const QString &name, const QString &pin, Done done) {
    const QString repository = entry->value("repository").toString();
    // Nothing is installed from it yet, unless this is an update being looked at.
    const bool following = entry->contains("installed");
    const auto resolved = [=](const Target &target, const QJsonObject &failure) {
        if (failed(failure)) {
            QJsonObject said = failure;
            if (!following)
                said.insert("error", failure.value("error").toString().remove(" The plugin that is installed is left as it is.")
                                         .replace("any more, or", "or").remove(" (any more)"));
            done(said);
            return;
        }
        // The commit rather than the name, which could move while it downloads.
        download(repository, target.commit.isEmpty() ? target.ref : target.commit, [=](const Reply &reply) {
            if (reply.status != 200 || reply.refused) {
                done(this->failure(reply, repository, following));
                return;
            }
            const QString file = QString::fromUtf8(reply.body);
            PluginTools::InstallOptions options;
            options.name = name;
            options.subfolder = entry->value("subfolder").toString();
            options.limits = m_limits;
            QJsonObject offered = PluginTools::inspect({m_folder, m_settings, {}}, file, options);
            if (failed(offered)) {
                done(QJsonObject{{"error", repository + " at " + target.ref + " is not a plugin Omabook can install: "
                                               + offered.value("error").toString()},
                                 {"gone", true}});
                return;
            }
            offered.insert("from", "github");
            offered.insert("repository", repository);
            offered.insert("subfolder", entry->value("subfolder"));
            offered.insert("channel", entry->value("channel"));
            offered.insert("ref", target.ref);
            offered.insert("commit", target.commit);
            m_looked = std::make_unique<Looked>(Looked{file, offered.value("name").toString(), entry, offered, target.json(), false});
            done(offered);
        });
    };
    if (pin.isEmpty()) {
        resolve(entry, resolved);
        return;
    }
    channelFor(entry, pin, [=](const QJsonObject &failure) {
        if (failed(failure))
            done(failure);
        else
            resolve(entry, resolved);
    });
}

void PluginSources::look(const QString &address, const QString &pin, const QString &name, Done done) {
    m_looked.reset();
    Address found;
    QString error;
    if (!parse(address, &found, &error)) {
        done(problem(error));
        return;
    }
    const Entry entry = std::make_shared<QJsonObject>(QJsonObject{{"repository", found.repository()},
                                                                  {"subfolder", found.subfolder},
                                                                  {"channel", QJsonObject{{"kind", "auto"}}}});
    const QString folder = name.isEmpty() ? nameFor(found) : name;
    if (!ScriptTools::validName(folder)) {
        done(problem(name.isEmpty() ? "No name for the plugin can be made of \"" + found.repo + "\"; give it one."
                                    : "\"" + name + "\" can't be a plugin's name: lower-case letters, digits, - and _."));
        return;
    }
    // A link to a tag or a branch says which to stay at; saying so does too.
    fetch(entry, folder, pin.isEmpty() ? found.ref : pin, done);
}

void PluginSources::dropLooked() { m_looked.reset(); }

QJsonObject PluginSources::put(const QString &name, const Entry &entry, const Target &target, const QString &file,
                               const QJsonObject &offered, bool enable, bool replace, bool update) {
    PluginTools::InstallOptions options;
    options.name = name;
    options.subfolder = entry->value("subfolder").toString();
    options.limits = m_limits;
    const QJsonObject before = entry->value("installed").toObject();
    if (update) {
        options.replace = true;
        options.keepState = true;
        options.previous = m_folder + "/.previous/" + name;
    } else {
        options.enable = enable;
        options.replace = replace;
    }
    QJsonObject answer = PluginTools::install({m_folder, m_settings, {}}, file, options);
    if (failed(answer))
        return answer;
    entry->insert("installed", QJsonObject{{"version", offered.value("version")},
                                           {"title", offered.value("title")},
                                           {"kind", target.kind},
                                           {"ref", target.ref},
                                           {"commit", target.commit},
                                           {"at", double(now())},
                                           {"permissions", offered.value("permissions")}});
    entry->insert("latest", target.json());
    entry->insert("checked_at", double(now()));
    for (const char *key : {"pending", "problem", "skip"})
        entry->remove(key);
    if (update && answer.value("replaced").toBool()) {
        entry->insert("previous", before);
    } else {
        entry->remove("previous");
        QDir(m_folder + "/.previous/" + name).removeRecursively();
    }
    store(name, *entry);
    answer.insert("from", "github");
    answer.insert("repository", entry->value("repository"));
    answer.insert("channel", entry->value("channel"));
    answer.insert("ref", target.ref);
    answer.insert("commit", target.commit);
    if (!update || !answer.value("enabled").toBool() || offered.value("kind").toString() != "script")
        return answer;

    // An update that doesn't load is taken back out at once. It is loaded
    // with no window and no library, as `omabook plugin check` does.
    const QJsonObject checked = ScriptTools::check({m_folder, m_settings, []() -> Library * { return nullptr; }}, name);
    if (checked.value("ok").toBool(true) && !failed(checked))
        return answer;
    const QString why = checked.value("error").toString();
    const QJsonObject back = rollBack(name);
    const QString said = offered.value("title").toString() + " " + offered.value("version").toString() + " didn't load (" + why + ")"
                         + (failed(back) ? ", and the version before it couldn't be put back: " + back.value("error").toString()
                                         : ", so Omabook went back to " + before.value("version").toString() + ".");
    QJsonObject entryNow = source(name);
    entryNow.insert("problem", said);
    store(name, entryNow);
    return {{"error", said}, {"name", name}, {"id", "folder:" + name}, {"updated", false}, {"rolledBack", !failed(back)}};
}

QJsonObject PluginSources::installLooked(bool enable, bool replace) {
    if (!m_looked)
        return problem("Nothing was fetched to install.");
    const Looked looked = *m_looked;
    const QJsonObject answer = put(looked.name, looked.entry, Target::from(looked.target), looked.file, looked.offered, enable, replace,
                                   looked.update);
    // Kept when it didn't go in, so the same choice can be made again.
    if (!failed(answer) || answer.contains("rolledBack"))
        m_looked.reset();
    if (failed(answer) || !looked.update)
        return answer;
    QJsonObject updated = answer;
    updated.insert("updated", true);
    updated.insert("from", looked.offered.value("fromVersion"));
    updated.insert("to", looked.offered.value("version"));
    return updated;
}

void PluginSources::add(const QString &address, const AddOptions &options, Done done) {
    look(address, options.pin, options.name, [=](const QJsonObject &offered) {
        if (failed(offered)) {
            done(offered);
            return;
        }
        if (offered.value("installed").toBool() && !options.replace) {
            dropLooked();
            done(problem("There already is a plugin " + offered.value("name").toString() + ". To put this one in its place (the one "
                         "there goes to the trash), say so: omabook plugin add --replace, or give it another --name."));
            return;
        }
        done(installLooked(options.enable, options.replace));
    });
}

// --- Keeping up to date ---

void PluginSources::check(const QString &name, Done done) {
    const Entry entry = std::make_shared<QJsonObject>(source(name));
    if (entry->isEmpty()) {
        done(problem("No source is remembered for a plugin " + name + "; see omabook plugin sources."));
        return;
    }
    if (!QFileInfo(m_folder + "/" + name).isDir()) {
        done(problem("The plugin " + name + " isn't installed any more. Forget its source: omabook plugin forget-source " + name));
        return;
    }
    QJsonObject answer{{"name", name}, {"id", "folder:" + name}, {"repository", entry->value("repository")},
                       {"channel", entry->value("channel")}, {"installed", entry->value("installed")},
                       {"title", entry->value("installed").toObject().value("title")}};
    if (entry->value("channel").toObject().value("kind").toString() == "tag") {
        // It stays where it was put: there is nothing to ask.
        answer.insert("pinned", true);
        answer.insert("update", false);
        done(answer);
        return;
    }
    resolve(entry, [=](const Target &target, const QJsonObject &failure) mutable {
        if (failed(failure)) {
            // What GitHub said of the source is remembered; that it couldn't
            // be asked is not the source's fault and is tried again.
            if (failure.value("gone").toBool()) {
                entry->insert("problem", failure.value("error"));
                entry->insert("checked_at", double(now()));
            }
            store(name, *entry);
            QJsonObject failedAnswer = failure;
            failedAnswer.insert("name", name);
            failedAnswer.insert("id", "folder:" + name);
            done(failedAnswer);
            return;
        }
        entry->insert("checked_at", double(now()));
        entry->remove("problem");
        // An update that waited is no longer the one to wait for when there is a newer.
        if (entry->value("pending").toObject().value("commit").toString() != target.commit)
            entry->remove("pending");
        store(name, *entry);
        answer.insert("latest", target.json());
        answer.insert("unchanged", target.unchanged);
        answer.insert("update", differs(entry->value("installed").toObject(), target.json()));
        if (entry->contains("pending"))
            answer.insert("pending", entry->value("pending"));
        done(answer);
    });
}

void PluginSources::apply(const QString &name, const Entry &entry, const Target &target, bool approved, Done done) {
    const QString repository = entry->value("repository").toString();
    download(repository, target.commit.isEmpty() ? target.ref : target.commit, [=](const Reply &reply) {
        QJsonObject failedAnswer{{"name", name}, {"id", "folder:" + name}, {"updated", false}};
        if (reply.status != 200 || reply.refused) {
            const QJsonObject failure = this->failure(reply, repository);
            if (failure.value("gone").toBool() || failure.value("refused").toBool()) {
                entry->insert("problem", failure.value("error"));
                store(name, *entry);
            }
            for (auto it = failure.begin(); it != failure.end(); ++it)
                failedAnswer.insert(it.key(), it.value());
            done(failedAnswer);
            return;
        }
        const QString file = QString::fromUtf8(reply.body);
        PluginTools::InstallOptions options;
        options.name = name;
        options.subfolder = entry->value("subfolder").toString();
        options.limits = m_limits;
        QJsonObject offered = PluginTools::inspect({m_folder, m_settings, {}}, file, options);
        if (failed(offered) && offered.contains("needsOmabook")) {
            // Written for a newer Omabook than this: the one installed
            // stays, and the update waits until Omabook itself is updated.
            const QJsonObject pending{{"kind", target.kind}, {"ref", target.ref}, {"commit", target.commit}, {"reason", "omabook"},
                                      {"needs", offered.value("needsOmabook")}};
            entry->insert("pending", pending);
            store(name, *entry);
            done(QJsonObject{{"name", name}, {"id", "folder:" + name}, {"updated", false}, {"waits", true}, {"pending", pending},
                             {"title", entry->value("installed").toObject().value("title")}});
            return;
        }
        if (failed(offered)) {
            // What is there now is no plugin: the one installed stays.
            const QString said = repository + " at " + target.ref + " is not a plugin Omabook can install (" + offered.value("error").toString()
                                 + "). The plugin that is installed is left as it is.";
            entry->insert("problem", said);
            store(name, *entry);
            failedAnswer.insert("error", said);
            failedAnswer.insert("gone", true);
            done(failedAnswer);
            return;
        }
        // Against what is on disk, which is what the person agreed to.
        const Manifest current(m_folder + "/" + name);
        QStringList asksMore;
        for (const QJsonValue &permission : offered.value("permissions").toArray())
            if (!current.permissions.contains(permission.toString()))
                asksMore << permission.toString();
        const bool renamed = offered.value("title").toString() != current.title;
        if ((!asksMore.isEmpty() || renamed) && !approved) {
            const QJsonObject pending{{"kind", target.kind}, {"ref", target.ref}, {"commit", target.commit},
                                      {"version", offered.value("version")}, {"title", offered.value("title")},
                                      {"permissions", QJsonArray::fromStringList(asksMore)},
                                      {"reason", asksMore.isEmpty() ? "name" : "permissions"}};
            entry->insert("pending", pending);
            store(name, *entry);
            done(QJsonObject{{"name", name}, {"id", "folder:" + name}, {"title", current.title}, {"updated", false}, {"review", true},
                             {"pending", pending}});
            return;
        }
        const QString from = entry->value("installed").toObject().value("version").toString();
        QJsonObject answer = put(name, entry, target, file, offered, false, true, true);
        if (!failed(answer)) {
            answer.insert("updated", true);
            answer.insert("from", from);
            answer.insert("to", offered.value("version"));
        }
        done(answer);
    });
}

void PluginSources::update(const QString &name, bool approved, Done done) {
    const Entry entry = std::make_shared<QJsonObject>(source(name));
    if (entry->isEmpty()) {
        done(problem("No source is remembered for a plugin " + name + "; see omabook plugin sources."));
        return;
    }
    if (!QFileInfo(m_folder + "/" + name).isDir()) {
        done(problem("The plugin " + name + " isn't installed any more. Forget its source: omabook plugin forget-source " + name));
        return;
    }
    resolve(entry, [=](const Target &target, const QJsonObject &failure) {
        if (failed(failure)) {
            if (failure.value("gone").toBool()) {
                entry->insert("problem", failure.value("error"));
                entry->insert("checked_at", double(now()));
            }
            store(name, *entry);
            QJsonObject failedAnswer = failure;
            failedAnswer.insert("name", name);
            failedAnswer.insert("id", "folder:" + name);
            failedAnswer.insert("updated", false);
            done(failedAnswer);
            return;
        }
        entry->insert("checked_at", double(now()));
        entry->remove("problem");
        store(name, *entry);
        if (!differs(entry->value("installed").toObject(), target.json())) {
            done(QJsonObject{{"name", name}, {"id", "folder:" + name}, {"updated", false}, {"upToDate", true},
                             {"title", entry->value("installed").toObject().value("title")},
                             {"version", entry->value("installed").toObject().value("version")}});
            return;
        }
        apply(name, entry, target, approved, done);
    });
}

void PluginSources::review(const QString &name, Done done) {
    m_looked.reset();
    const Entry entry = std::make_shared<QJsonObject>(source(name));
    if (entry->isEmpty() || !QFileInfo(m_folder + "/" + name).isDir()) {
        done(problem("No source is remembered for a plugin " + name + "."));
        return;
    }
    const QString pin; // the channel is the source's own
    const QJsonObject installed = entry->value("installed").toObject();
    fetch(entry, name, pin, [=](const QJsonObject &offered) {
        if (failed(offered) || !m_looked) {
            done(offered);
            return;
        }
        const Manifest current(m_folder + "/" + name);
        QStringList asksMore;
        for (const QJsonValue &permission : offered.value("permissions").toArray())
            if (!current.permissions.contains(permission.toString()))
                asksMore << permission.toString();
        QJsonObject described = offered;
        described.insert("update", true);
        described.insert("fromVersion", installed.value("version"));
        described.insert("newPermissions", QJsonArray::fromStringList(asksMore));
        described.insert("renamedFrom", offered.value("title").toString() != current.title ? current.title : QString());
        m_looked->offered = described;
        m_looked->update = true;
        done(described);
    });
}

void PluginSources::pin(const QString &name, const QString &ref, Done done) {
    const Entry entry = std::make_shared<QJsonObject>(source(name));
    if (entry->isEmpty() || !QFileInfo(m_folder + "/" + name).isDir()) {
        done(problem("No source is remembered for a plugin " + name + "."));
        return;
    }
    if (ref.trimmed().isEmpty()) {
        done(problem("Name the tag or branch to stay at."));
        return;
    }
    channelFor(entry, ref.trimmed(), [=](const QJsonObject &failure) {
        if (failed(failure)) {
            done(failure);
            return;
        }
        for (const char *key : {"etags", "latest", "pending", "skip"})
            entry->remove(key);
        resolve(entry, [=](const Target &target, const QJsonObject &failure) {
            // A ref that isn't there changes nothing: the source follows what it did.
            if (failed(failure)) {
                done(failure);
                return;
            }
            entry->insert("checked_at", double(now()));
            entry->remove("problem");
            store(name, *entry);
            if (!differs(entry->value("installed").toObject(), target.json())) {
                done(QJsonObject{{"name", name}, {"id", "folder:" + name}, {"channel", entry->value("channel")}, {"updated", false},
                                 {"upToDate", true}});
                return;
            }
            apply(name, entry, target, false, [=](const QJsonObject &answer) {
                QJsonObject pinned = answer;
                pinned.insert("channel", entry->value("channel"));
                done(pinned);
            });
        });
    });
}

void PluginSources::checkAll(bool apply, bool dueOnly, Done done) {
    auto names = std::make_shared<QStringList>();
    for (const QJsonValue &value : list()) {
        const QJsonObject entry = value.toObject();
        if (entry.value("missing").toBool() || entry.value("channel").toObject().value("kind").toString() == "tag")
            continue;
        if (dueOnly && now() - qint64(entry.value("checked_at").toDouble()) < checkEvery)
            continue;
        *names << entry.value("name").toString();
    }
    auto results = std::make_shared<QJsonArray>();
    auto next = std::make_shared<std::function<void()>>();
    *next = [=] {
        if (names->isEmpty()) {
            done(QJsonObject{{"results", *results}});
            *next = nullptr;
            return;
        }
        const QString name = names->takeFirst();
        const auto took = [=](const QJsonObject &answer) {
            *results << answer;
            // Asked to wait, or not reached: the rest would fare no better.
            if (answer.value("limited").toBool() || answer.value("offline").toBool()) {
                done(QJsonObject{{"results", *results}, {"limited", answer.value("limited").toBool()},
                                 {"offline", answer.value("offline").toBool()}});
                *next = nullptr;
                return;
            }
            // From the event loop, so a long list doesn't grow the stack.
            QTimer::singleShot(0, this, [=] {
                if (*next)
                    (*next)();
            });
        };
        check(name, [=](const QJsonObject &checked) {
            // Not what was gone back from, and not what already waits for
            // the person or for a newer Omabook: those aren't fetched again.
            const QJsonObject entry = source(name);
            const QString there = checked.value("latest").toObject().value("commit").toString();
            const bool wanted = apply && checked.value("update").toBool() && entry.value("skip").toString() != there
                                && entry.value("pending").toObject().value("commit").toString() != there;
            if (wanted)
                update(name, false, took);
            else
                took(checked);
        });
    };
    (*next)();
}

QJsonObject PluginSources::wait(const std::function<void(Done)> &start, int milliseconds) {
    QEventLoop loop;
    auto answer = std::make_shared<QJsonObject>();
    auto answered = std::make_shared<bool>(false);
    // An answer that comes after it was given up on finds no loop to stop.
    const QPointer<QEventLoop> waiting(&loop);
    start([waiting, answer, answered](const QJsonObject &given) {
        *answer = given;
        *answered = true;
        if (waiting)
            waiting->quit();
    });
    if (!*answered) {
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return *answered ? *answer : problem("GitHub took too long to answer, so it was given up on.");
}

// The same, as tools for an agent talking to `omabook mcp`.
namespace {

Mcp::Result told(const QJsonObject &answer) {
    return failed(answer) ? Mcp::Result::fail(answer.value("error").toString()) : Mcp::Result::ok(answer);
}

const bool toolsRegistered = [] {
    const QString nameIs = "The plugin's name, which is its folder's, from plugin_sources.";
    bool ok = Mcp::registerTool({
        "plugin_sources", "Where plugins came from",
        "The plugins that were installed from a GitHub repository: the repository and folder, what each follows (the "
        "latest release, a branch, or a tag it stays at), the version and commit installed, when it was last checked, "
        "and whether an update is there, waits for the user's approval, or the source has a problem. Asks nothing of GitHub.",
        Mcp::schema(), Mcp::ReadOnly,
        [](const QJsonObject &) {
            const PluginSources sources{PluginTools::Context{}};
            return Mcp::Result::ok({{"folder", sources.folder()}, {"sources", sources.list()}});
        },
    });
    Mcp::Tool add{
        "plugin_add", "Install a plugin from GitHub",
        "Installs a plugin from a public GitHub repository and remembers where it came from, so it can be kept up to "
        "date. The address is owner/repo, a link to the repository, or a link to a release, tag, branch or folder of "
        "it. The plugin is fetched over HTTPS and checked like any other (plugin_install). It is installed switched off "
        "unless `enable` is true; show the user the permissions in the answer before enabling it.",
        Mcp::schema({{"address", Mcp::string("The repository: owner/repo, or a GitHub link.")},
                     {"name", Mcp::string("The folder name to install it under; taken from the repository when not given.")},
                     {"pin", Mcp::string("A tag to stay at, or a branch to follow, instead of the latest release.")},
                     {"enable", Mcp::boolean("Switch it on at once. False when not given.")},
                     {"replace", Mcp::boolean("Replace a plugin of the same name, which goes to the trash. False when not given.")}},
                    {"address"}),
        Mcp::Changes,
        [](const QJsonObject &arguments) {
            PluginSources sources{PluginTools::Context{}};
            PluginSources::AddOptions options;
            options.name = arguments.value("name").toString();
            options.pin = arguments.value("pin").toString();
            options.enable = arguments.value("enable").toBool();
            options.replace = arguments.value("replace").toBool();
            return told(PluginSources::wait(
                [&](PluginSources::Done done) { sources.add(arguments.value("address").toString(), options, done); }));
        },
    };
    add.openWorld = true;
    ok = Mcp::registerTool(add) && ok;
    Mcp::Tool update{
        "plugin_update", "Check or update plugins from GitHub",
        "Checks the source of one plugin (`name`), or of all of them, for something newer than what is installed, and "
        "installs it unless `check` is true. The plugin keeps its data and stays on or off as it was, and the version "
        "before is kept for one step back. An update that asks for a permission the installed version doesn't have is "
        "not installed: the answer says `review` with what it asks for, and only `approve`, after the user agreed, "
        "installs it. One that doesn't load is taken back out.",
        Mcp::schema({{"name", Mcp::string("The plugin to check; every plugin with a source when not given.")},
                     {"check", Mcp::boolean("Only say whether there is something newer. False when not given.")},
                     {"approve", Mcp::boolean("Also install an update that asks for more permissions. Only after the user agreed.")}}),
        Mcp::Changes,
        [](const QJsonObject &arguments) {
            PluginSources sources{PluginTools::Context{}};
            const QString name = arguments.value("name").toString();
            const bool only = arguments.value("check").toBool(), approved = arguments.value("approve").toBool();
            return told(PluginSources::wait([&](PluginSources::Done done) {
                if (name.isEmpty())
                    sources.checkAll(!only, false, done);
                else if (only)
                    sources.check(name, done);
                else
                    sources.update(name, approved, done);
            }));
        },
    };
    update.openWorld = true;
    ok = Mcp::registerTool(update) && ok;
    ok = Mcp::registerTool({
        "plugin_forget_source", "Stop updating a plugin",
        "Forgets where a plugin came from, so it is no longer checked or updated. The plugin stays installed as it is.",
        Mcp::schema({{"name", Mcp::string(nameIs)}}, {"name"}), Mcp::Changes,
        [](const QJsonObject &arguments) {
            return told(PluginSources{PluginTools::Context{}}.forget(arguments.value("name").toString()));
        },
    }) && ok;
    return ok;
}();

} // namespace
