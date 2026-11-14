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
      m_opener([](const QUrl &url) { return QDesktopServices::openUrl(url); }),
      m_sources({registry->folder(), settings, {}}, network) {
    m_daily.setSingleShot(true);
    connect(&m_daily, &QTimer::timeout, this, &PluginManager::dailyCheck);
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
    // The daily look at the sources, once the window is up and busy with
    // nothing else. It asks nothing when no plugin came from a repository.
    checkSoon(3000);
}

PluginManager::~PluginManager() = default;

QString PluginManager::folder() const { return m_registry->folder(); }

QStringList PluginManager::templates() const { return ScriptApi::templates(); }

void PluginManager::refresh() {
    // Each plugin that came from a repository says so, and what of the
    // sources needs attention joins what of the plugins does.
    const QJsonArray overview = m_sources.annotate(PluginTools::overview(m_registry->known(), m_scripts));
    const QVariantList plugins = overview.toVariantList();
    QJsonArray issues = PluginTools::health(overview, m_registry->calibre());
    for (const QJsonValue &issue : m_sources.issues())
        issues << issue;
    const QVariantList health = issues.toVariantList();
    bool sourced = false;
    for (const QJsonValue &row : overview)
        sourced = sourced || row.toObject().contains("source");
    if (plugins == m_plugins && health == m_health && sourced == m_sourced)
        return;
    m_plugins = plugins;
    m_health = health;
    m_sourced = sourced;
    emit changed();

    // An update of this run that stopped with an error after it was loaded
    // in the window (a panel, a command) is taken back out, as one that
    // doesn't load at all is.
    for (const QString &name : QStringList(m_updated)) {
        const QJsonObject row = [&] {
            for (const QJsonValue &value : overview)
                if (value.toObject().value("id").toString() == "folder:" + name)
                    return value.toObject();
            return QJsonObject();
        }();
        if (row.value("state").toString() != "broken" || !row.value("source").toObject().value("previous").toBool())
            continue;
        m_updated.removeAll(name);
        const QJsonObject back = m_sources.rollBack(name);
        if (ScriptTools::failed(back))
            continue;
        const QString text = tr("%1 stopped with an error after its update, so Omabook went back to version %2.")
                                 .arg(row.value("name").toString(), back.value("version").toString());
        note("folder:" + name, text, true);
        emit said(text);
        m_scripts->reload();
        QTimer::singleShot(0, this, &PluginManager::refresh);
    }
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
    if (m_offer.value("from").toString() == "github") {
        // What was fetched from a repository goes in the same way, and where
        // it came from is remembered. An update keeps the plugin's switch.
        const bool update = m_offer.value("update").toBool();
        const QVariantMap answer = m_sources.installLooked(enable, replace).toVariantMap();
        if (answer.contains("error") && !answer.contains("rolledBack")) {
            m_offer.insert("error", answer.value("error"));
            emit offerChanged();
            return answer;
        }
        m_offer.clear();
        emit offerChanged();
        const QString name = answer.value("name").toString();
        if (answer.contains("error"))
            note("folder:" + name, answer.value("error").toString(), true);
        else if (update)
            note("folder:" + name, tr("Updated from %1 to %2.").arg(answer.value("from").toString(), answer.value("to").toString()));
        if (update && !answer.contains("error") && !m_updated.contains(name))
            m_updated << name;
        m_scripts->reload();
        refresh();
        emit touched();
        return answer;
    }
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
    m_sources.dropLooked();
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

// --- Plugins from GitHub repositories ---

void PluginManager::setChecking(const QString &id) {
    if (m_checking == id)
        return;
    m_checking = id;
    emit busyChanged();
}

void PluginManager::note(const QString &id, const QString &text, bool problem) {
    m_sourceNote = {{"id", id}, {"text", text}, {"problem", problem}};
    emit sourceNoteChanged();
}

void PluginManager::lookUp(const QString &address) {
    if (m_fetching || address.trimmed().isEmpty())
        return;
    m_fetching = true;
    emit busyChanged();
    if (!m_offer.isEmpty()) {
        m_offer.clear();
        emit offerChanged();
    }
    m_sources.look(address, {}, {}, [this](const QJsonObject &offered) {
        m_fetching = false;
        emit busyChanged();
        m_offer = offered.toVariantMap();
        emit offerChanged();
    });
}

QString PluginManager::outcome(const QJsonObject &answer, bool asked) {
    const QString title = answer.value("title").toString(answer.value("name").toString());
    if (answer.value("updated").toBool())
        return tr("%1 was updated from %2 to %3.").arg(title, answer.value("from").toString(), answer.value("to").toString());
    if (answer.value("waits").toBool())
        return tr("%1 has an update that needs Omabook %2 or newer; the version installed stays.")
            .arg(title, answer.value("pending").toObject().value("needs").toString());
    if (answer.value("review").toBool())
        return tr("%1 has an update that asks for more than the version installed. Review it under Plugins.").arg(title);
    // That GitHub wasn't reached, or asked to wait, is only said to someone who asked.
    if (ScriptTools::failed(answer))
        return asked || answer.value("gone").toBool() || answer.contains("rolledBack") ? answer.value("error").toString() : QString();
    if (answer.value("update").toBool())
        return tr("%1 has an update: %2. Install it under Plugins.").arg(title, answer.value("latest").toObject().value("ref").toString());
    return asked ? tr("%1 is up to date.").arg(title) : QString();
}

void PluginManager::sourceAnswered(const QJsonObject &answer, bool asked) {
    const QString name = answer.value("name").toString();
    const QString text = outcome(answer, asked);
    if (answer.value("updated").toBool() && !m_updated.contains(name))
        m_updated << name;
    if (!text.isEmpty() && !name.isEmpty())
        note("folder:" + name, text, ScriptTools::failed(answer));
    // What happened by itself is said where it is seen; what was asked for
    // is answered beside the plugin.
    const bool worthSaying = answer.value("updated").toBool() || answer.value("review").toBool() || answer.contains("rolledBack")
                             || answer.value("update").toBool();
    if (!asked && worthSaying && !text.isEmpty())
        emit said(text);
}

void PluginManager::checkNow(const QString &id) {
    if (!m_checking.isEmpty())
        return;
    setChecking(id);
    m_sources.check(id.section(':', 1), [this](const QJsonObject &answer) {
        setChecking({});
        sourceAnswered(answer, true);
        refresh();
    });
}

void PluginManager::updateNow(const QString &id) {
    if (!m_checking.isEmpty())
        return;
    setChecking(id);
    m_sources.update(id.section(':', 1), false, [this, id](const QJsonObject &answer) {
        setChecking({});
        sourceAnswered(answer, true);
        m_scripts->reload();
        refresh();
        emit touched();
        // One that asks for more is shown, for the person to say yes or no to.
        if (answer.value("review").toBool())
            reviewUpdate(id);
    });
}

void PluginManager::reviewUpdate(const QString &id) {
    if (m_fetching)
        return;
    m_fetching = true;
    emit busyChanged();
    m_sources.review(id.section(':', 1), [this](const QJsonObject &offered) {
        m_fetching = false;
        emit busyChanged();
        m_offer = offered.toVariantMap();
        emit offerChanged();
    });
}

void PluginManager::pinTo(const QString &id, const QString &ref) {
    if (!m_checking.isEmpty())
        return;
    setChecking(id);
    m_sources.pin(id.section(':', 1), ref, [this, id](const QJsonObject &answer) {
        setChecking({});
        QJsonObject named = answer;
        named.insert("name", id.section(':', 1));
        sourceAnswered(named, true);
        m_scripts->reload();
        refresh();
        emit touched();
        if (answer.value("review").toBool())
            reviewUpdate(id);
    });
}

void PluginManager::followLatest(const QString &id) {
    if (ScriptTools::failed(m_sources.follow(id.section(':', 1))))
        return;
    refresh();
    updateNow(id);
}

void PluginManager::rollBack(const QString &id) {
    const QString name = id.section(':', 1);
    const QJsonObject answer = m_sources.rollBack(name);
    m_updated.removeAll(name);
    note(id, ScriptTools::failed(answer) ? answer.value("error").toString()
                                         : tr("Back at version %1.").arg(answer.value("version").toString()),
         ScriptTools::failed(answer));
    m_scripts->reload();
    refresh();
    emit touched();
}

void PluginManager::forgetSource(const QString &id) {
    m_sources.forget(id.section(':', 1));
    refresh();
}

void PluginManager::checkAll() {
    if (!m_checking.isEmpty())
        return;
    setChecking("all");
    // By hand, what is newer is installed unless updates are only to be told of.
    m_sources.checkAll(m_settings->pluginUpdates() == "auto", false, [this](const QJsonObject &answer) {
        setChecking({});
        const QJsonArray results = answer.value("results").toArray();
        for (const QJsonValue &result : results)
            sourceAnswered(result.toObject(), true);
        if (results.size() != 1)
            note("all", answer.value("limited").toBool() || answer.value("offline").toBool()
                            ? results.last().toObject().value("error").toString()
                            : tr("%Ln sources were checked.", "", int(results.size())),
                 answer.value("limited").toBool() || answer.value("offline").toBool());
        m_scripts->reload();
        refresh();
        emit touched();
    });
}

void PluginManager::checkSoon(int milliseconds) { m_daily.start(milliseconds); }

void PluginManager::dailyCheck() {
    const QString mode = m_settings->pluginUpdates();
    // Nothing is asked of GitHub when no plugin came from there, when the
    // person said never to, or when everything was checked within the day.
    if (mode == "never" || !m_checking.isEmpty() || !m_sources.anyDue())
        return;
    setChecking("daily");
    m_sources.checkAll(mode == "auto", true, [this](const QJsonObject &answer) {
        setChecking({});
        for (const QJsonValue &result : answer.value("results").toArray())
            sourceAnswered(result.toObject(), false);
        m_scripts->reload();
        refresh();
        emit touched();
    });
}
