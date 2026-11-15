#include "pluginfinder.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>

#include "mcp.h"
#include "scriptapi.h"
#include "settings.h"

namespace {

QJsonObject failure(const QString &why) { return {{"error", why}}; }

// The registry is in Omabook's own repository, on its main branch.
constexpr auto registryPath = "/jellespijker/omabook/main/share/omabook/plugins.json";
constexpr auto topic = "omabook-plugin";

const QStringList &kindNames() {
    static const QStringList kinds{"format", "command", "panel", "metadata", "catalog", "importer", "hook", "launcher", "form", "program"};
    return kinds;
}

bool versionLike(const QString &text) {
    static const QRegularExpression version("^\\d+(\\.\\d+){0,3}$");
    return version.match(text).hasMatch();
}

QJsonObject parsedObject(const QByteArray &bytes, QString *error) {
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError) {
        *error = QString("it isn't valid JSON: %1 (at character %2)").arg(parse.errorString()).arg(parse.offset);
        return {};
    }
    if (!document.isObject()) {
        *error = "it isn't a JSON object";
        return {};
    }
    return document.object();
}

QString words(const QJsonObject &entry) {
    QStringList all{entry.value("name").toString(), entry.value("title").toString(), entry.value("description").toString(),
                    entry.value("author").toString(), entry.value("repository").toString()};
    for (const QJsonValue &kind : entry.value("kinds").toArray())
        all << kind.toString();
    return all.join('\n');
}

bool matches(const QJsonObject &entry, const QString &query) {
    const QString all = words(entry);
    const QStringList asked = query.split(' ', Qt::SkipEmptyParts);
    return std::ranges::all_of(asked, [&all](const QString &word) { return all.contains(word, Qt::CaseInsensitive); });
}

// "2026-10-01T12:00:00Z" as the day it was.
QString day(const QString &stamp) { return stamp.left(10); }

} // namespace

PluginFinder::PluginFinder(const PluginTools::Context &context, QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_context(context), m_sources(std::make_unique<PluginSources>(context, network, this)),
      m_clock([] { return QDateTime::currentSecsSinceEpoch(); }), m_cacheFile(m_sources->folder() + "/plugin-finder.json") {}

PluginFinder::~PluginFinder() = default;

void PluginFinder::setService(const PluginSources::Service &service) { m_sources->setService(service); }

void PluginFinder::setClock(std::function<qint64()> clock) {
    m_clock = clock;
    m_sources->setClock(std::move(clock));
}

bool PluginFinder::discovery() const { return m_discovery >= 0 ? m_discovery == 1 : m_sources->m_settings->pluginDiscovery(); }

// --- The registry ---

QJsonObject PluginFinder::shipped() {
    QFile file(":/plugins.json");
    QString error;
    return file.open(QIODevice::ReadOnly) ? parsedObject(file.readAll(), &error) : QJsonObject();
}

QString PluginFinder::problem(const QJsonObject &registry) {
    if (registry.value("version").toInt() != 1)
        return "The registry is not version 1.";
    if (!registry.value("plugins").isArray())
        return "The registry has no list of plugins.";
    static const QRegularExpression repository("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$");
    QSet<QString> names;
    int index = 0;
    for (const QJsonValue &value : registry.value("plugins").toArray()) {
        ++index;
        const QJsonObject entry = value.toObject();
        const QString name = entry.value("name").toString();
        const QString at = QString("Plugin %1 of the registry (%2)").arg(index).arg(name.isEmpty() ? QString("no name") : name);
        if (!value.isObject())
            return QString("Plugin %1 of the registry is not an object.").arg(index);
        if (!ScriptTools::validName(name))
            return at + " has no proper name: lower-case letters, digits, - and _.";
        if (names.contains(name))
            return "The registry lists " + name + " twice.";
        names.insert(name);
        for (const char *field : {"title", "description", "author", "license"})
            if (!entry.value(field).isString() || entry.value(field).toString().trimmed().isEmpty())
                return at + " has no " + field + ".";
        if (!repository.match(entry.value("repository").toString()).hasMatch())
            return at + " names no repository as owner/name.";
        if (entry.contains("subfolder") && !entry.value("subfolder").isString())
            return at + " has a subfolder that is not text.";
        if (!versionLike(entry.value("omabook").toString()))
            return at + " says no version of Omabook it needs.";
        if (!entry.value("permissions").isArray())
            return at + " lists no permissions (an empty list is fine).";
        for (const QJsonValue &permission : entry.value("permissions").toArray())
            if (!ScriptApi::permissionNames().contains(permission.toString()))
                return at + " asks for the permission \"" + permission.toString() + "\", which there isn't.";
        if (!entry.value("kinds").isArray() || entry.value("kinds").toArray().isEmpty())
            return at + " says nothing of what kind of plugin it is.";
        for (const QJsonValue &kind : entry.value("kinds").toArray())
            if (!kindNames().contains(kind.toString()))
                return at + " is of the kind \"" + kind.toString() + "\"; the kinds are: " + kindNames().join(", ") + ".";
        if (!entry.value("reviewed").isBool())
            return at + " doesn't say whether it was reviewed.";
    }
    return {};
}

QJsonObject PluginFinder::registry() const {
    const QJsonObject fetched = readCache().value("registry").toObject().value("body").toObject();
    return !fetched.isEmpty() && problem(fetched).isEmpty() ? fetched : shipped();
}

QJsonObject PluginFinder::listed(const QString &name) const {
    for (const QJsonValue &value : registry().value("plugins").toArray())
        if (value.toObject().value("name").toString() == name)
            return value.toObject();
    return {};
}

QString PluginFinder::addressOf(const QJsonObject &entry) {
    const QString repository = entry.value("repository").toString(), subfolder = entry.value("subfolder").toString();
    if (subfolder.isEmpty())
        return repository;
    return "https://github.com/" + repository + "/tree/" + entry.value("ref").toString("main") + "/" + subfolder;
}

void PluginFinder::refresh(bool force, Done done) {
    QJsonObject cache = readCache();
    QJsonObject kept = cache.value("registry").toObject();
    const auto answer = [this](QJsonObject what) {
        what.insert("plugins", registry().value("plugins").toArray().size());
        return what;
    };
    // With discovery off, nothing is asked of the network.
    if (!discovery()) {
        done(answer({{"refreshed", false}, {"unchanged", true}, {"problem", "Looking for plugins on GitHub is switched off."}}));
        return;
    }
    if (!force && now() < qint64(kept.value("next").toDouble())) {
        done(answer({{"refreshed", false}, {"unchanged", true}}));
        return;
    }
    QHash<QByteArray, QByteArray> headers{{"Accept", "application/json"}};
    if (!kept.value("etag").toString().isEmpty() && !kept.value("body").toObject().isEmpty())
        headers.insert("If-None-Match", kept.value("etag").toString().toUtf8());
    const QUrl url(m_sources->m_service.raw + registryPath);
    m_sources->get(url, headers, {}, 0, false, [this, answer, done](const PluginSources::Reply &reply) {
        QJsonObject cache = readCache();
        QJsonObject kept = cache.value("registry").toObject();
        QJsonObject outcome;
        if (reply.status == 304) {
            kept.insert("next", double(now() + PluginSources::checkEvery));
            outcome = {{"refreshed", false}, {"unchanged", true}};
        } else if (reply.status == 200) {
            QString error;
            const QJsonObject fetched = parsedObject(reply.body, &error);
            const QString wrong = error.isEmpty() ? problem(fetched) : "The registry on GitHub can't be read: " + error + ".";
            // A registry that doesn't pass is not taken; the one there was stays.
            if (wrong.isEmpty()) {
                kept.insert("body", fetched);
                kept.insert("etag", QString::fromUtf8(reply.headers.value("etag")));
                kept.remove("problem");
                outcome = {{"refreshed", true}, {"unchanged", false}};
            } else {
                kept.insert("problem", wrong);
                outcome = {{"refreshed", false}, {"unchanged", false}, {"problem", wrong}};
            }
            kept.insert("next", double(now() + PluginSources::checkEvery));
        } else {
            // Offline, or GitHub said no: asked again in an hour, quietly.
            const QString why = reply.error.isEmpty() ? QString("GitHub answered %1 for the registry.").arg(reply.status) : reply.error;
            kept.insert("next", double(now() + 60 * 60));
            outcome = {{"refreshed", false}, {"unchanged", false}, {"offline", reply.offline}, {"problem", why}};
        }
        kept.insert("checked_at", double(now()));
        cache.insert("registry", kept);
        writeCache(cache);
        done(answer(outcome));
    });
}

// --- Searching ---

QJsonArray PluginFinder::matching(const QString &query) const {
    QJsonArray found;
    for (const QJsonValue &value : registry().value("plugins").toArray()) {
        QJsonObject entry = value.toObject();
        if (!matches(entry, query))
            continue;
        entry.insert("listed", true);
        entry.insert("address", addressOf(entry));
        entry.insert("subfolder", entry.value("subfolder").toString());
        const QString needs = ScriptApi::needsNewer(entry.value("omabook"));
        entry.insert("installable", needs.isEmpty());
        if (!needs.isEmpty())
            entry.insert("problem", needs);
        found << installedState(entry);
    }
    return found;
}

QJsonObject PluginFinder::installedState(QJsonObject result) const {
    const QString name = result.value("name").toString();
    bool installed = !name.isEmpty() && QFileInfo(m_sources->folder() + "/" + name + "/plugin.json").isFile();
    // Installed under another name is installed too: the source says so.
    for (const QJsonValue &value : m_sources->list()) {
        const QJsonObject source = value.toObject();
        if (!source.value("missing").toBool()
            && source.value("repository").toString().compare(result.value("repository").toString(), Qt::CaseInsensitive) == 0
            && source.value("subfolder").toString() == result.value("subfolder").toString()) {
            installed = true;
            result.insert("installedAs", source.value("name").toString());
        }
    }
    result.insert("installed", installed);
    return result;
}

void PluginFinder::search(const QString &query, Done done) {
    const QString words = query.simplified();
    refresh(false, [this, words, done](const QJsonObject &) {
        const QJsonArray listed = matching(words);
        if (!discovery()) {
            done({{"results", listed}, {"skipped", QJsonArray()}, {"discovery", false}, {"limited", false}});
            return;
        }
        discover(words, listed, done);
    });
}

bool PluginFinder::waiting(QString *why) const {
    const qint64 until = qint64(readCache().value("wait_until").toDouble());
    if (until <= now())
        return false;
    *why = "GitHub asked Omabook to wait until " + QDateTime::fromSecsSinceEpoch(until).toString("HH:mm")
           + " before searching again (it allows few searches a minute); the registry is shown meanwhile.";
    return true;
}

void PluginFinder::wait(const PluginSources::Reply &reply) {
    // GitHub says until when, one way or another; a minute when it doesn't.
    qint64 until = now() + 60;
    if (const qint64 after = reply.headers.value("retry-after").toLongLong(); after > 0)
        until = now() + after;
    else if (const qint64 reset = reply.headers.value("x-ratelimit-reset").toLongLong(); reset > now())
        until = reset;
    QJsonObject cache = readCache();
    cache.insert("wait_until", double(std::min(until, now() + 60 * 60)));
    writeCache(cache);
}

void PluginFinder::discover(const QString &words, QJsonArray listed, Done done) {
    const QString key = words.toLower();
    const QJsonObject kept = readCache().value("searches").toObject().value(key).toObject();
    // Merges what the topic search found into the listed ones: a listed
    // plugin gets the repository's stars and date, and is not shown twice.
    const auto finish = [this, done](QJsonArray listed, const QJsonArray &found, const QJsonArray &skipped, bool limited, const QString &why) {
        QJsonArray results;
        QSet<QString> seen;
        for (const QJsonValue &value : found) {
            const QJsonObject one = value.toObject();
            bool merged = false;
            for (qsizetype i = 0; i < listed.size(); ++i) {
                QJsonObject entry = listed.at(i).toObject();
                if (entry.value("repository").toString().compare(one.value("repository").toString(), Qt::CaseInsensitive) != 0)
                    continue;
                for (const char *field : {"stars", "updated", "archived"})
                    entry.insert(field, one.value(field));
                listed[i] = entry;
                merged = true;
            }
            if (!merged && !seen.contains(one.value("repository").toString().toLower())) {
                seen.insert(one.value("repository").toString().toLower());
                results << installedState(one);
            }
        }
        QJsonArray all = listed;
        for (const QJsonValue &value : std::as_const(results))
            all << value;
        QJsonObject answer{{"results", all}, {"skipped", skipped}, {"discovery", true}, {"limited", limited}};
        if (!why.isEmpty())
            answer.insert("problem", why);
        done(answer);
    };
    if (!kept.isEmpty() && now() < qint64(kept.value("at").toDouble()) + keepSearch) {
        finish(listed, kept.value("found").toArray(), kept.value("skipped").toArray(), false, {});
        return;
    }
    QString why;
    if (waiting(&why)) {
        finish(listed, kept.value("found").toArray(), kept.value("skipped").toArray(), true, why);
        return;
    }
    const auto items = std::make_shared<QJsonArray>();
    topicPage(words, 1, items, [this, words, key, listed, items, finish](const QString &problem, bool limited) {
        if (!problem.isEmpty() && items->isEmpty()) {
            const QJsonObject before = readCache().value("searches").toObject().value(key).toObject();
            finish(listed, before.value("found").toArray(), before.value("skipped").toArray(), limited, problem);
            return;
        }
        const auto offered = std::make_shared<QJsonArray>(), skipped = std::make_shared<QJsonArray>();
        examine(items, 0, offered, skipped, [this, key, listed, offered, skipped, finish, problem, limited] {
            QJsonObject cache = readCache();
            QJsonObject searches = cache.value("searches").toObject();
            searches.insert(key, QJsonObject{{"at", double(now())}, {"found", *offered}, {"skipped", *skipped}});
            // A few searches are kept, the latest.
            while (searches.size() > 20) {
                QString oldest;
                double at = 0;
                for (auto it = searches.begin(); it != searches.end(); ++it)
                    if (oldest.isEmpty() || it.value().toObject().value("at").toDouble() < at)
                        oldest = it.key(), at = it.value().toObject().value("at").toDouble();
                searches.remove(oldest);
            }
            cache.insert("searches", searches);
            writeCache(cache);
            finish(listed, *offered, *skipped, limited, problem);
        });
    });
}

void PluginFinder::topicPage(const QString &words, int page, std::shared_ptr<QJsonArray> found,
                             std::function<void(const QString &problem, bool limited)> done) {
    QUrl url(m_sources->m_service.api + "/search/repositories");
    QUrlQuery query;
    query.addQueryItem("q", QString("topic:%1%2").arg(topic, words.isEmpty() ? QString() : " " + words));
    query.addQueryItem("per_page", "30");
    query.addQueryItem("page", QString::number(page));
    url.setQuery(query);
    const QHash<QByteArray, QByteArray> headers{{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    m_sources->get(url, headers, {}, 0, false, [this, words, page, found, done](const PluginSources::Reply &reply) {
        if (reply.status == 403 || reply.status == 429) {
            wait(reply);
            QString why;
            waiting(&why);
            done(why, true);
            return;
        }
        if (reply.status != 200) {
            done(reply.error.isEmpty() ? QString("GitHub's search answered %1.").arg(reply.status) : reply.error, false);
            return;
        }
        QString error;
        const QJsonObject answer = parsedObject(reply.body, &error);
        const QJsonArray items = answer.value("items").toArray();
        for (const QJsonValue &item : items)
            if (found->size() < mostCandidates)
                *found << item;
        // Pages until there are no more, or as many as are looked into.
        if (!items.isEmpty() && found->size() < mostCandidates && found->size() < answer.value("total_count").toInt() && page < 3) {
            topicPage(words, page + 1, found, done);
            return;
        }
        done({}, false);
    });
}

void PluginFinder::examine(std::shared_ptr<QJsonArray> candidates, int at, std::shared_ptr<QJsonArray> offered,
                           std::shared_ptr<QJsonArray> skipped, std::function<void()> done) {
    if (at >= candidates->size()) {
        done();
        return;
    }
    const QJsonObject item = candidates->at(at).toObject();
    const auto next = [this, candidates, at, offered, skipped, done] { examine(candidates, at + 1, offered, skipped, done); };
    const QString repository = item.value("full_name").toString();
    const auto skip = [skipped, repository, next](const QString &why) {
        *skipped << QJsonObject{{"repository", repository}, {"problem", why}};
        next();
    };
    if (item.value("archived").toBool()) {
        skip("It is archived on GitHub, so nobody keeps it up any more.");
        return;
    }
    PluginSources::Address address;
    QString error;
    if (!PluginSources::parse(repository, &address, &error)) {
        skip(error);
        return;
    }
    const QString name = PluginSources::nameFor(address);
    manifestOf(item, {"plugin.json", name + "/plugin.json"}, [item, repository, name, skip, offered, next](
                                                                 const QJsonObject &manifest, const QString &path) {
        if (manifest.contains("error")) {
            skip(manifest.value("error").toString());
            return;
        }
        if (manifest.value("name").toString().trimmed().isEmpty()) {
            skip("Its plugin.json gives the plugin no name.");
            return;
        }
        bool malformed = false;
        const QString needs = ScriptApi::needsNewer(manifest.value("omabook"), &malformed);
        if (!needs.isEmpty()) {
            skip(needs);
            return;
        }
        QStringList permissions;
        for (const QJsonValue &permission : manifest.value("permissions").toArray()) {
            if (!ScriptApi::permissionNames().contains(permission.toString())) {
                skip("Its plugin.json asks for the permission \"" + permission.toString() + "\", which there isn't.");
                return;
            }
            permissions << permission.toString();
        }
        const QString subfolder = path.contains('/') ? path.section('/', 0, -2) : QString();
        const QString branch = item.value("default_branch").toString("main");
        *offered << QJsonObject{
            {"name", name},
            {"title", manifest.value("name").toString().simplified()},
            {"description", manifest.value("description").toString(item.value("description").toString()).simplified()},
            {"repository", repository},
            {"subfolder", subfolder},
            {"address", subfolder.isEmpty() ? repository : "https://github.com/" + repository + "/tree/" + branch + "/" + subfolder},
            {"author", manifest.value("author").toString().trimmed().isEmpty() ? item.value("owner").toObject().value("login").toString()
                                                                              : manifest.value("author").toString()},
            {"license", item.value("license").toObject().value("spdx_id").toString()},
            {"permissions", QJsonArray::fromStringList(permissions)},
            {"program", manifest.value("script").toString().isEmpty()},
            {"omabook", manifest.value("omabook").toString()},
            {"version", manifest.value("version").toString()},
            {"kinds", QJsonArray()},
            {"listed", false},
            {"reviewed", false},
            {"stars", item.value("stargazers_count").toInt()},
            {"updated", day(item.value("pushed_at").toString(item.value("updated_at").toString()))},
            {"archived", false},
            {"installable", true},
        };
        next();
    });
}

void PluginFinder::manifestOf(const QJsonObject &repository, const QStringList &paths,
                              std::function<void(const QJsonObject &, const QString &)> done) {
    const QString name = repository.value("full_name").toString(), pushed = repository.value("pushed_at").toString();
    // A repository not pushed to since it was last looked at says the same.
    const QJsonObject known = readCache().value("manifests").toObject().value(name.toLower()).toObject();
    if (!known.isEmpty() && !pushed.isEmpty() && known.value("pushed_at").toString() == pushed) {
        done(known.value("manifest").toObject(), known.value("path").toString());
        return;
    }
    const auto remember = [this, name, pushed, done](const QJsonObject &manifest, const QString &path) {
        QJsonObject cache = readCache();
        QJsonObject manifests = cache.value("manifests").toObject();
        manifests.insert(name.toLower(), QJsonObject{{"pushed_at", pushed}, {"manifest", manifest}, {"path", path}});
        cache.insert("manifests", manifests);
        writeCache(cache);
        done(manifest, path);
    };
    if (paths.isEmpty()) {
        remember({{"error", "It has no plugin.json at its root or in a folder named after the plugin."}}, {});
        return;
    }
    QUrl url(m_sources->m_service.api + "/repos/" + name + "/contents/" + paths.first());
    QUrlQuery query;
    query.addQueryItem("ref", repository.value("default_branch").toString("main"));
    url.setQuery(query);
    const QHash<QByteArray, QByteArray> headers{{"Accept", "application/vnd.github.raw"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    m_sources->get(url, headers, {}, 0, false, [this, repository, paths, done, remember](const PluginSources::Reply &reply) {
        if (reply.status == 404) {
            manifestOf(repository, paths.mid(1), done);
            return;
        }
        if (reply.status == 403 || reply.status == 429)
            wait(reply);
        if (reply.status != 200) {
            // Not remembered: next time it may be answered.
            done({{"error", reply.error.isEmpty() ? QString("GitHub answered %1 for its plugin.json.").arg(reply.status) : reply.error}}, {});
            return;
        }
        QString error;
        const QJsonObject manifest = parsedObject(reply.body, &error);
        if (!error.isEmpty()) {
            remember({{"error", "Its plugin.json can't be read: " + error + "."}}, {});
            return;
        }
        remember(manifest, paths.first());
    });
}

// --- What is kept ---

QJsonObject PluginFinder::readCache() const {
    QFile file(m_cacheFile);
    QString error;
    return file.open(QIODevice::ReadOnly) ? parsedObject(file.readAll(), &error) : QJsonObject();
}

void PluginFinder::writeCache(const QJsonObject &cache) const {
    QDir().mkpath(QFileInfo(m_cacheFile).absolutePath());
    QSaveFile file(m_cacheFile);
    file.setDirectWriteFallback(false);
    const QByteArray contents = QJsonDocument(cache).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size())
        file.commit();
}

// The same, as a tool for an agent talking to `omabook mcp`.
namespace {

const bool toolRegistered = [] {
    Mcp::Tool search{
        "plugin_search", "Find plugins to install",
        "Finds plugins for Omabook: those in the registry kept with Omabook (marked listed; `reviewed` when the "
        "maintainer looked at one, which is not a guarantee), and repositories on GitHub with the topic "
        "omabook-plugin, each checked to have a valid plugin.json that fits this Omabook (marked not listed, not "
        "reviewed). Each says what it may do (its permissions), its repository, stars, licence and whether it is "
        "installed. Install one with plugin_add, giving its name or `address`, and show the user the permissions first. "
        "With the setting plugin_discovery off, only the registry this Omabook was built with is searched.",
        Mcp::schema({{"words", Mcp::string("Words to look for in a plugin's name, title and description; every plugin when not given.")}}),
        Mcp::ReadOnly,
        [](const QJsonObject &arguments) {
            PluginFinder finder{PluginTools::Context{}};
            return Mcp::Result::of(PluginSources::wait(
                [&finder, &arguments](PluginSources::Done done) { finder.search(arguments.value("words").toString(), done); }));
        },
    };
    search.openWorld = true;
    return Mcp::registerTool(search);
}();

} // namespace
