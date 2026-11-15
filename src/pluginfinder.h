#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

#include "pluginsources.h"

// Finding plugins to install, in two layers.
//
// The registry: share/omabook/plugins.json in Omabook's own repository,
// compiled into the binary and read from GitHub again at most once a day, so
// a plugin listed after this Omabook was built is found without an update.
// Each entry is a plugin someone asked to have listed, by a pull request
// that adds it; `reviewed` says the maintainer looked at it, which is not a
// guarantee. A registry that doesn't pass the schema check is refused and
// the last good one stays.
//
//   {"version": 1, "plugins": [{"name", "title", "description",
//     "repository": "owner/repo", "subfolder", "ref", "author", "license",
//     "permissions": [...], "omabook": "0.1.4", "kinds": [...], "reviewed"}]}
//
// Discovery: repositories on GitHub with the topic "omabook-plugin", found
// with GitHub's search, each looked at before it is offered: its plugin.json
// (just that file) must be there and valid, at the root or in a folder named
// after the plugin, the Omabook it asks for must not be newer than this one,
// and the repository must not be archived. Search allows few questions a
// minute, so what was found is kept for a while, and a "wait" from GitHub is
// kept to. The setting `plugin_discovery` switches this layer off; the
// registry is then all there is, and with no network at all, the shipped one.
//
// What is kept between runs (the registry as last fetched, with its ETag,
// and recent searches) is in plugin-finder.json beside sources.json.
//
// Every answer is an object, and one with an "error" (English) failed.
class PluginFinder : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(const QJsonObject &answer)>;

    explicit PluginFinder(const PluginTools::Context &context, QNetworkAccessManager *network = nullptr, QObject *parent = nullptr);
    ~PluginFinder() override;

    void setService(const PluginSources::Service &service);
    void setClock(std::function<qint64()> clock);
    // Whether GitHub's search is asked at all; the settings say, unless told here.
    void setDiscovery(bool on) { m_discovery = on ? 1 : 0; }
    bool discovery() const;

    // The registry compiled into this Omabook.
    static QJsonObject shipped();
    // Why a registry can't be used, in English; empty when it can.
    static QString problem(const QJsonObject &registry);
    // The registry in use: the last good one fetched, else the shipped one.
    QJsonObject registry() const;
    // A listed plugin by its name, or an empty object.
    QJsonObject listed(const QString &name) const;
    // The address `omabook plugin add` installs a listed plugin from.
    static QString addressOf(const QJsonObject &entry);

    // Fetches the registry from GitHub when it is a day old (or `force`):
    // {refreshed, unchanged, offline, plugins}. Offline, or refused, it says
    // why in "problem" and the registry in use stays; that is not a failure.
    void refresh(bool force, Done done);

    // Plugins that match the words (every one for none): {results, skipped,
    // discovery, limited, problem}. Each result is
    //   {name, title, description, repository, subfolder, address, author,
    //    license, permissions, omabook, kinds, listed, reviewed, stars,
    //    updated, archived, installed, installable, problem}
    // with the listed ones first. `skipped` are the repositories with the
    // topic that were looked at and aren't offered, each with why.
    void search(const QString &words, Done done);

    // How long a search is kept, and the most repositories looked into for one.
    static constexpr qint64 keepSearch = 15 * 60;
    static constexpr int mostCandidates = 20;

private:
    QJsonObject readCache() const;
    void writeCache(const QJsonObject &cache) const;
    qint64 now() const { return m_clock(); }
    QJsonArray matching(const QString &words) const;
    void discover(const QString &words, QJsonArray listed, Done done);
    void topicPage(const QString &words, int page, std::shared_ptr<QJsonArray> found, std::function<void(const QString &problem, bool limited)> done);
    void examine(std::shared_ptr<QJsonArray> candidates, int at, std::shared_ptr<QJsonArray> offered, std::shared_ptr<QJsonArray> skipped,
                 std::function<void()> done);
    void manifestOf(const QJsonObject &repository, const QStringList &paths, std::function<void(const QJsonObject &, const QString &)> done);
    QJsonObject installedState(QJsonObject result) const;
    bool waiting(QString *why) const;
    void wait(const PluginSources::Reply &reply);

    PluginTools::Context m_context;
    std::unique_ptr<PluginSources> m_sources; // for its careful requests, and what is installed
    std::function<qint64()> m_clock;
    int m_discovery = -1; // -1: as the settings say
    QString m_cacheFile;
};
