#include "pluginmanager.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include "metadatasource.h"
#include "plugins.h"
#include "plugintools.h"
#include "scriptapi.h"
#include "scriptplugins.h"
#include "settings.h"

PluginManager::PluginManager(Settings *settings, Plugins *registry, ScriptPlugins *scripts, FilePicker *picker,
                             QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_settings(settings), m_registry(registry), m_scripts(scripts), m_picker(picker), m_network(network),
      m_opener([](const QUrl &url) { return QDesktopServices::openUrl(url); }) {
    connect(m_picker, &FilePicker::picked, this, &PluginManager::picked);
    // The order of the sources and every switch live in the settings, which
    // may be edited by hand or by an agent while the window is up.
    connect(m_settings, &Settings::changed, this, &PluginManager::refresh);
    // A source that never answers is given up on, so "Try it" always ends.
    m_trialLimit.setSingleShot(true);
    m_trialLimit.setInterval(30000);
    connect(&m_trialLimit, &QTimer::timeout, this, [this] {
        if (!m_trial.value("asking").toBool())
            return;
        const QVariantMap kept = m_trial;
        stopTrial();
        m_trial = kept;
        m_trial.insert("asking", false);
        m_trial.insert("answered", true);
        m_trial.insert("problem", tr("It gave no answer in half a minute."));
        emit trialChanged();
    });
    refresh();
}

PluginManager::~PluginManager() = default;

QString PluginManager::folder() const { return m_registry->folder(); }

QStringList PluginManager::templates() const { return ScriptApi::templates(); }

void PluginManager::refresh() {
    const QJsonArray overview = PluginTools::overview(m_registry->known(), m_scripts);
    const QVariantList plugins = overview.toVariantList();
    const QVariantList health = PluginTools::health(overview, m_registry->calibre()).toVariantList();
    if (plugins == m_plugins && health == m_health)
        return;
    m_plugins = plugins;
    m_health = health;
    emit changed();
}

void PluginManager::reload() {
    m_scripts->reload();
    refresh();
    emit touched();
}

QString PluginManager::folderName(const QString &text) const { return PluginTools::folderName(text); }

QVariantMap PluginManager::create(const QString &name, const QString &templateName) {
    const QJsonObject answer = ScriptTools::create({m_registry->folder(), m_settings, {}}, name, templateName);
    if (ScriptTools::failed(answer))
        return answer.toVariantMap();
    // Loaded now rather than when the folder's change is noticed, so the
    // list has it by the time the dialog looks.
    m_scripts->reload();
    refresh();
    emit touched();
    QVariantMap made = answer.toVariantMap();
    made.insert("id", "folder:" + name);
    // A new plugin is something to edit: its folder is where that happens.
    openFolder("folder:" + name);
    return made;
}

void PluginManager::pick(bool folder) { m_picker->pick(folder ? FilePicker::InstallPluginFolder : FilePicker::InstallPlugin); }

void PluginManager::picked(FilePicker::Purpose purpose, const QList<QUrl> &urls) {
    if ((purpose != FilePicker::InstallPlugin && purpose != FilePicker::InstallPluginFolder) || urls.isEmpty())
        return;
    consider(urls.first().toLocalFile());
}

void PluginManager::consider(const QString &source) {
    m_offer = PluginTools::inspect({m_registry->folder(), m_settings, {}}, source).toVariantMap();
    emit offerChanged();
}

QVariantMap PluginManager::install(bool enable, bool replace) {
    const QString source = m_offer.value("source").toString();
    if (source.isEmpty())
        return {{"error", tr("There is no plugin waiting to be installed.")}};
    PluginTools::InstallOptions options;
    options.name = m_offer.value("name").toString();
    options.enable = enable;
    options.replace = replace;
    const QVariantMap answer = PluginTools::install({m_registry->folder(), m_settings, {}}, source, options).toVariantMap();
    if (answer.contains("error")) {
        // What it is stays on show, with why it didn't go in.
        m_offer.insert("error", answer.value("error"));
        emit offerChanged();
        return answer;
    }
    m_offer.clear();
    emit offerChanged();
    m_scripts->reload();
    refresh();
    emit touched();
    return answer;
}

void PluginManager::dismissOffer() {
    if (m_offer.isEmpty())
        return;
    m_offer.clear();
    emit offerChanged();
}

QString PluginManager::pluginFolder(const QString &id) const {
    for (const QVariant &value : m_plugins) {
        const QVariantMap plugin = value.toMap();
        if (plugin.value("id").toString() == id)
            return plugin.value("folder").toString();
    }
    return {};
}

QVariantMap PluginManager::remove(const QString &id) {
    // Only what is in the plugin folder: the rest can be switched off.
    if (pluginFolder(id).isEmpty())
        return {{"error", tr("Only your own plugins can be removed.")}};
    if (m_trial.value("id").toString() == id)
        stopTrial();
    const QVariantMap answer = PluginTools::remove({m_registry->folder(), m_settings, {}}, id.section(':', 1)).toVariantMap();
    m_scripts->reload();
    refresh();
    emit touched();
    return answer;
}

void PluginManager::openFolder(const QString &id) {
    const QString folder = id.isEmpty() ? m_registry->folder() : pluginFolder(id);
    if (!folder.isEmpty() && QFileInfo(folder).isDir())
        m_opener(QUrl::fromLocalFile(folder));
}

void PluginManager::retry(const QString &id) {
    const QString name = id.section(':', 1);
    PluginTools::dismissError(m_registry->folder(), name);
    m_scripts->retry(name);
    refresh();
    emit touched();
}

void PluginManager::dismissError(const QString &id) {
    PluginTools::dismissError(m_registry->folder(), id.section(':', 1));
    refresh();
    emit touched();
}

bool PluginManager::move(const QString &id, int by) {
    if (!m_registry->move(id, by))
        return false;
    refresh();
    emit touched();
    return true;
}

void PluginManager::tryOut(const QString &id, const QString &title) {
    stopTrial();
    PluginInfo wanted;
    for (const PluginInfo &plugin : m_registry->known())
        if (plugin.id == id)
            wanted = plugin;
    const QString asked = title.simplified();
    if (wanted.id.isEmpty() || asked.isEmpty())
        return;

    m_trialSources = new QObject(this);
    QList<MetadataSource *> sources;
    if (wanted.kind == "script") {
        // The sources this one script registered, among every script's.
        const ScriptPlugin *running = m_scripts->plugin(id.section(':', 1));
        QStringList names;
        for (const ScriptPlugin::Source &source : running ? running->sources() : QList<ScriptPlugin::Source>())
            names << source.name;
        for (MetadataSource *source : m_scripts->metadataSources(m_trialSources)) {
            if (names.contains(source->name()))
                sources << source;
            else
                delete source;
        }
    } else {
        sources = m_registry->sources({wanted}, m_network, m_trialSources);
    }

    m_trial = {{"id", id}, {"title", asked}, {"asking", !sources.isEmpty()}, {"answered", sources.isEmpty()},
               {"problem", sources.isEmpty() ? tr("It is off, or can't be used as it is, so there is nothing to ask.") : QString()},
               {"results", QVariantList()}};
    m_trialPending = int(sources.size());
    emit trialChanged();
    if (sources.isEmpty())
        return;

    BookMeta book;
    book.title = asked;
    // The sample is asked for with its author, as a real look-up would be.
    if (asked == sampleTitle())
        book.authors = QStringList{"Jane Austen"};
    for (MetadataSource *source : std::as_const(sources)) {
        connect(source, &MetadataSource::found, this,
                [this, source](const QList<MetaCandidate> &candidates) { answered(source, candidates, {}); });
        connect(source, &MetadataSource::failed, this, [this, source](const QString &problem) { answered(source, {}, problem); });
    }
    m_trialLimit.start();
    for (MetadataSource *source : std::as_const(sources))
        source->search(book);
}

void PluginManager::answered(MetadataSource *source, const QList<MetaCandidate> &candidates, const QString &problem) {
    // A source answers once; one that says more is not listened to again.
    source->disconnect(this);
    QVariantList results = m_trial.value("results").toList();
    for (const MetaCandidate &candidate : candidates.mid(0, 5)) {
        // What it filled in, which is what taking it would bring to a book that has nothing.
        results << QVariantMap{{"source", candidate.source},
                               {"title", candidate.meta.title},
                               {"authors", candidate.meta.authorText()},
                               {"published", candidate.meta.published},
                               {"brings", whatItBrings(BookMeta(), false, candidate).adds}};
    }
    m_trial.insert("results", results);
    if (!problem.isEmpty()) {
        const QString before = m_trial.value("problem").toString();
        m_trial.insert("problem", before.isEmpty() ? problem : before + "\n" + problem);
    }
    if (--m_trialPending <= 0) {
        m_trialLimit.stop();
        m_trial.insert("asking", false);
        m_trial.insert("answered", true);
    }
    emit trialChanged();
}

void PluginManager::stopTrial() {
    m_trialLimit.stop();
    if (m_trialSources) {
        // The sources may be the ones calling this, from far up the stack.
        for (QObject *source : m_trialSources->children())
            source->disconnect(this);
        m_trialSources->deleteLater();
        m_trialSources = nullptr;
    }
    m_trialPending = 0;
    if (m_trial.isEmpty())
        return;
    m_trial.clear();
    emit trialChanged();
}
