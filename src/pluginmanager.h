#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "filepicker.h"
#include "pluginsources.h"

class MetadataSource;
class Plugins;
class ScriptPlugins;
class Settings;
struct MetaCandidate;

// The plugin manager as the window uses it: every plugin sorted by what it
// does, what needs attention, and everything that can be done to one. It
// holds no knowledge of its own; that is PluginTools', which the command
// line and the MCP server share, and the registry's. It knows nothing of the
// window either, so tests drive it with none.
class PluginManager : public QObject {
    Q_OBJECT
    // Every plugin, as PluginTools::overview describes it.
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY changed)
    // What needs attention: [{id, name, issue}], see PluginTools::health.
    Q_PROPERTY(QVariantList health READ health NOTIFY changed)
    Q_PROPERTY(QString folder READ folder CONSTANT)
    // The templates a new plugin starts from, by name.
    Q_PROPERTY(QStringList templates READ templates CONSTANT)
    // A plugin that was picked to install and waits to be approved: what
    // PluginTools::inspect said of it, or {error}. Empty when there is none.
    Q_PROPERTY(QVariantMap offer READ offer NOTIFY offerChanged)
    // One look-up tried against one plugin: {id, title, asking, answered,
    // problem, results: [{source, title, authors, published, brings}]}.
    Q_PROPERTY(QVariantMap trial READ trial NOTIFY trialChanged)
    // A book every catalog knows, to try a source with.
    Q_PROPERTY(QString sampleTitle READ sampleTitle CONSTANT)
    // Plugins from GitHub repositories (see PluginSources). `fetching` while
    // an address is being looked up for the install sheet; `checking` holds
    // the id of the plugin whose source is being asked, or "all"; `sourced`
    // when any plugin has a source at all; `sourceNote` is what the last
    // check or update of one plugin came to: {id, text, problem}.
    Q_PROPERTY(bool fetching READ fetching NOTIFY busyChanged)
    Q_PROPERTY(QString checking READ checking NOTIFY busyChanged)
    Q_PROPERTY(bool sourced READ sourced NOTIFY changed)
    Q_PROPERTY(QVariantMap sourceNote READ sourceNote NOTIFY sourceNoteChanged)

public:
    // What opens a folder in the file manager; tests pass their own.
    using Opener = std::function<bool(const QUrl &)>;

    PluginManager(Settings *settings, Plugins *registry, ScriptPlugins *scripts, FilePicker *picker,
                  QNetworkAccessManager *network, QObject *parent = nullptr);
    ~PluginManager() override;

    void setOpener(Opener opener) { m_opener = std::move(opener); }

    QVariantList plugins() const { return m_plugins; }
    QVariantList health() const { return m_health; }
    QString folder() const;
    QStringList templates() const;
    QVariantMap offer() const { return m_offer; }
    QVariantMap trial() const { return m_trial; }
    QString sampleTitle() const { return QStringLiteral("Pride and Prejudice"); }
    bool fetching() const { return m_fetching; }
    QString checking() const { return m_checking; }
    bool sourced() const { return m_sourced; }
    QVariantMap sourceNote() const { return m_sourceNote; }
    PluginSources *sources() { return &m_sources; }

    // Looks at the plugins again. The registry is asked what is known, so
    // this is cheap enough to do whenever something may have changed.
    Q_INVOKABLE void refresh();
    // Reads the plugin folder again without waiting to be told it changed.
    Q_INVOKABLE void reload();

    // A folder name made of whatever was typed; empty when none can be.
    Q_INVOKABLE QString folderName(const QString &text) const;
    // Starts a plugin from a template and shows its folder: {name, id,
    // folder}, or {error}.
    Q_INVOKABLE QVariantMap create(const QString &name, const QString &templateName);
    // Asks for a .zip, or for a folder, to install from; the answer is `offer`.
    Q_INVOKABLE void pick(bool folder);
    // Looks at a plugin to install without asking where it is.
    Q_INVOKABLE void consider(const QString &source);
    // Installs the plugin on offer, switched on or left off: {id, ...} or {error}.
    Q_INVOKABLE QVariantMap install(bool enable, bool replace = false);
    Q_INVOKABLE void dismissOffer();
    // Moves one of the user's own plugins to the trash: {id, ...} or {error}.
    Q_INVOKABLE QVariantMap remove(const QString &id);
    // Shows a plugin's folder, or the plugin folder itself for "".
    Q_INVOKABLE void openFolder(const QString &id = {});
    // Loads a script plugin again, and forgets its last error.
    Q_INVOKABLE void retry(const QString &id);
    Q_INVOKABLE void dismissError(const QString &id);
    // Moves a source of details earlier (-1) or later (1) in the order asked.
    Q_INVOKABLE bool move(const QString &id, int by);

    // Fetches the plugin at a GitHub address (owner/repo, or a link) and
    // puts it on offer, as `pick` does for a .zip; `install` then installs
    // it and remembers where it came from.
    Q_INVOKABLE void lookUp(const QString &address);
    // Asks a plugin's source whether there is something newer.
    Q_INVOKABLE void checkNow(const QString &id);
    // Installs what is newer. An update that asks for more than the version
    // installed is put on offer instead, for the person to approve.
    Q_INVOKABLE void updateNow(const QString &id);
    Q_INVOKABLE void reviewUpdate(const QString &id);
    // Stays at a tag or follows a branch; follows the latest release again;
    // goes back to the version before the last update; stops updating.
    Q_INVOKABLE void pinTo(const QString &id, const QString &ref);
    Q_INVOKABLE void followLatest(const QString &id);
    Q_INVOKABLE void rollBack(const QString &id);
    Q_INVOKABLE void forgetSource(const QString &id);
    // Every source now, as the daily look does by itself.
    Q_INVOKABLE void checkAll();
    // The daily look, `milliseconds` from now: sources not checked in the
    // last day are asked, and what the settings say is done about updates.
    // It is all answered later, so nothing waits on it.
    void checkSoon(int milliseconds);

    // Asks one plugin about one title, to see that it works.
    Q_INVOKABLE void tryOut(const QString &id, const QString &title);
    Q_INVOKABLE void stopTrial();

signals:
    void changed();
    void offerChanged();
    void trialChanged();
    void busyChanged();
    void sourceNoteChanged();
    // Something the person should hear of though they didn't ask: a plugin
    // that was updated, or one that went back to the version before.
    void said(const QString &text);
    // The manager changed what there is (installed, removed, reordered), so
    // whoever else shows the plugins should look again.
    void touched();

private:
    void picked(FilePicker::Purpose purpose, const QList<QUrl> &urls);
    void answered(MetadataSource *source, const QList<MetaCandidate> &candidates, const QString &problem);
    QString pluginFolder(const QString &id) const;
    void setChecking(const QString &id);
    void note(const QString &id, const QString &text, bool problem = false);
    // What a check or an update of one source came to, in words.
    QString outcome(const QJsonObject &answer, bool asked);
    void sourceAnswered(const QJsonObject &answer, bool asked);
    void dailyCheck();

    Settings *m_settings;
    Plugins *m_registry;
    ScriptPlugins *m_scripts;
    FilePicker *m_picker;
    QNetworkAccessManager *m_network;
    Opener m_opener;
    QVariantList m_plugins, m_health;
    QVariantMap m_offer, m_trial;
    // The sources of the look-up being tried, and how many have yet to answer.
    QObject *m_trialSources = nullptr;
    int m_trialPending = 0;
    QTimer m_trialLimit;
    PluginSources m_sources;
    QTimer m_daily;
    bool m_fetching = false, m_sourced = false;
    QString m_checking;
    QVariantMap m_sourceNote;
    // Plugins updated in this run: one that then stops with an error goes
    // back to the version before by itself.
    QStringList m_updated;
};
