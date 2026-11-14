#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <functional>
#include <memory>

#include "plugintools.h"

// Plugins that come from a GitHub repository, and are kept up to date from
// it. Omabook needs no git for this: it asks GitHub's API which commit a
// release, a branch or a tag stands at, downloads that commit as a tarball
// over HTTPS, and hands it to the same checked install a .zip goes through
// (PluginTools::inspect and install). Nothing of a plugin runs before the
// person said it may.
//
// What was installed from where is remembered in `sources.json` in the
// plugin folder, beside the plugins it is about:
//
//   {"version": 1, "wait_until": 0,
//    "sources": {"storyteller": {
//        "repository": "jellespijker/omabook-storyteller", "subfolder": "",
//        "channel": {"kind": "auto" | "branch" | "tag", "name": "main"},
//        "installed": {"version", "title", "kind", "ref", "commit", "at", "permissions"},
//        "latest": {"kind", "ref", "commit"},     what the last check saw
//        "checked_at": 0, "etags": {"<path>": "<etag>"}, "default_branch": "main",
//        "pending": {...},    an update that waits for the person: it asks for more
//        "previous": {...},   the version before this one, kept for one step back
//        "skip": "<commit>",  one that was gone back from, and isn't taken again by itself
//        "problem": "..."}}}  why the source can't be followed, when it can't
//
// It is a file of its own and not part of config.json because it is what
// Omabook keeps track of, not what the person chose: it is written at every
// check, and config.json is a file people edit, sync and watch. What they do
// choose (whether updates install themselves) is in the settings.
//
// A channel is what a source follows: "auto" is the latest release where the
// repository has releases and its default branch where it has none; "branch"
// follows the head of one branch; "tag" stays at one tag and never updates.
//
// Every answer is an object, and one with an "error" (English) failed. The
// calls that ask GitHub answer later, through `Done`; `wait` turns one into
// a call that returns, for the command line and the MCP server.
class PluginSources : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(const QJsonObject &answer)>;

    // A repository as it was named: any of
    //   owner/repo                  https://github.com/owner/repo(.git)
    //   git@github.com:owner/repo.git
    //   .../tree/<ref>[/sub/folder] .../blob/<ref>/sub/folder/plugin.json
    //   .../releases/tag/<tag>      .../releases[/latest]      .../commit/<sha>
    // A ref with a slash in it can't be told from a subfolder in an address;
    // give it with `pin` instead.
    struct Address {
        QString owner, repo;
        QString ref;       // the branch, tag or commit named, or empty
        QString subfolder; // where plugin.json is, or empty for the root
        QString repository() const { return owner + "/" + repo; }
    };
    static bool parse(const QString &text, Address *address, QString *error);
    // The folder a plugin from a repository is installed in when nobody says:
    // the repository's name without a leading "omabook-" or "omabook-plugin-",
    // or the last part of its subfolder.
    static QString nameFor(const Address &address);

    // Where GitHub is. Tests, and OMABOOK_GITHUB_API naming an address on
    // this computer, put a stand-in in its place.
    struct Service {
        QString api = "https://api.github.com";
        // The only hosts a request may be redirected to.
        QStringList hosts{"github.com", "api.github.com", "codeload.github.com", "objects.githubusercontent.com"};
        bool httpsOnly = true;
        static Service fromEnvironment();
    };

    explicit PluginSources(const PluginTools::Context &context, QNetworkAccessManager *network = nullptr, QObject *parent = nullptr);
    ~PluginSources() override;

    void setService(const Service &service) { m_service = service; }
    void setLimits(const PluginTools::Limits &limits) { m_limits = limits; }
    // The time, in seconds since the epoch; tests move it.
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }
    QString folder() const { return m_folder; }
    // How long a source is left alone after it was checked, when nobody asked.
    static constexpr qint64 checkEvery = 24 * 60 * 60;

    // --- What is remembered; none of these asks GitHub ---

    // Every source: the entry above with "name", "id", "update" (the last
    // check saw something newer than what is installed) and "missing" (the
    // plugin's folder is gone).
    QJsonArray list() const;
    QJsonObject source(const QString &name) const;
    // The plugin stays; it just stops being updated.
    QJsonObject forget(const QString &name);
    // Follows the latest release, or the default branch, again.
    QJsonObject follow(const QString &name);
    // Back to the version before the last update: {name, version, from}.
    // What the plugin kept in data/ meanwhile stays with it.
    QJsonObject rollBack(const QString &name);
    // An overview (PluginTools::overview) with each sourced plugin's
    // "source", and what of the sources needs attention, for the health line:
    //   {id, name, issue: "review", permissions, reason}  an update that asks for more waits
    //   {id, name, issue: "update-needs-omabook", needs}  an update waits for a newer Omabook
    //   {id, name, issue: "update", version}              an update is there, and wasn't taken
    //   {id, name, issue: "source", problem}              the source can't be followed
    QJsonArray annotate(const QJsonArray &overview) const;
    QJsonArray issues() const;
    // Whether any source is due for the daily look.
    bool anyDue() const;

    // --- Asking GitHub ---

    // Fetches the plugin at an address and says what it is, without
    // installing it: PluginTools::inspect's answer with {from: "github",
    // repository, subfolder, channel, ref, commit}. `pin` is a tag or branch
    // to stay at. What was fetched is kept until `installLooked` or the next look.
    void look(const QString &address, const QString &pin, const QString &name, Done done);
    // Installs what `look` fetched, and remembers where it came from.
    QJsonObject installLooked(bool enable, bool replace = false);
    void dropLooked();
    // Both at once: {.., id, folder, enabled}.
    struct AddOptions {
        QString name, pin;
        bool enable = false, replace = false;
    };
    void add(const QString &address, const AddOptions &options, Done done);

    // Whether there is something newer: {name, repository, installed,
    // latest, update, pinned, unchanged}. A pinned source isn't asked.
    void check(const QString &name, Done done);
    // Checks, and installs what is newer: {updated, from, to, title}. An
    // update that asks for a permission the installed version doesn't have,
    // or goes by another name, is only installed when `approved`; otherwise
    // it is remembered as pending: {updated: false, review: true, pending}.
    // One that doesn't load is taken back out: {updated: false, rolledBack: true, error}.
    // One written for a newer Omabook than this waits: {updated: false, waits: true, pending}.
    void update(const QString &name, bool approved, Done done);
    // Fetches the update that waits for a source and describes it as `look`
    // does, with {update: true, name, newPermissions, reason, from}, for the
    // person to approve; `installLooked` then installs it.
    void review(const QString &name, Done done);
    // Stays at a tag, or follows a branch, and goes to it now.
    void pin(const QString &name, const QString &ref, Done done);
    // Every source in turn: {results: [...], limited}. With `dueOnly`, only
    // those not checked in the last day; with `apply`, what is newer is
    // installed as `update` does without approval.
    void checkAll(bool apply, bool dueOnly, Done done);

    // Runs one of the above to its answer, for a caller with no event loop
    // of its own. {error} when it took longer than `milliseconds`.
    static QJsonObject wait(const std::function<void(Done)> &start, int milliseconds = 180000);

private:
    using Entry = std::shared_ptr<QJsonObject>;
    struct Reply;
    struct Target;
    using Answered = std::function<void(const Reply &reply)>;
    using Resolved = std::function<void(const Target &target, const QJsonObject &failure)>;

    QJsonObject readAll() const;
    bool writeAll(const QJsonObject &all) const;
    void store(const QString &name, const QJsonObject &entry) const;
    qint64 now() const { return m_clock(); }
    QString token(const QUrl &url);

    void get(const QUrl &url, const QHash<QByteArray, QByteArray> &headers, const QString &intoFile, int hops, bool redirected,
             Answered done);
    void api(const Entry &entry, const QString &path, const QByteArray &accept, Answered done);
    void download(const QString &repository, const QString &ref, Answered done);
    QJsonObject failure(const Reply &reply, const QString &repository, bool installed = true) const;

    void resolve(const Entry &entry, Resolved done);
    void commitOf(const Entry &entry, const QString &kind, const QString &ref, Resolved done);
    void branchHead(const Entry &entry, bool again, Resolved done);
    void channelFor(const Entry &entry, const QString &ref, std::function<void(const QJsonObject &failure)> done);
    void fetch(const Entry &entry, const QString &name, const QString &pin, Done done);
    void apply(const QString &name, const Entry &entry, const Target &target, bool approved, Done done);
    QJsonObject put(const QString &name, const Entry &entry, const Target &target, const QString &file, const QJsonObject &offered,
                    bool enable, bool replace, bool update);

    QString m_folder;
    Settings *m_settings;
    std::unique_ptr<Settings> m_ownSettings;
    QNetworkAccessManager *m_network;
    Service m_service;
    PluginTools::Limits m_limits;
    std::function<qint64()> m_clock;
    // Where tarballs are downloaded to; emptied with this object.
    std::unique_ptr<QTemporaryDir> m_downloads;
    int m_downloadCount = 0;
    // What `look` fetched and `installLooked` installs.
    struct Looked {
        QString file, name;
        Entry entry;
        QJsonObject offered, target;
        bool update = false;
    };
    std::unique_ptr<Looked> m_looked;
    QString m_token;
    bool m_tokenRead = false;
};
