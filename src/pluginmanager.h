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

    // Asks one plugin about one title, to see that it works.
    Q_INVOKABLE void tryOut(const QString &id, const QString &title);
    Q_INVOKABLE void stopTrial();

signals:
    void changed();
    void offerChanged();
    void trialChanged();
    // The manager changed what there is (installed, removed, reordered), so
    // whoever else shows the plugins should look again.
    void touched();

private:
    void picked(FilePicker::Purpose purpose, const QList<QUrl> &urls);
    void answered(MetadataSource *source, const QList<MetaCandidate> &candidates, const QString &problem);
    QString pluginFolder(const QString &id) const;

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
};
