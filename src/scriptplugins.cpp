#include "scriptplugins.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include "ai.h"
#include "formatprovider.h"
#include "settings.h"

namespace {

// A book as the scripts are told of it: what `omabook list --json` prints.
QJsonObject told(const Book &book) {
    QJsonObject object = book.toJson();
    object.insert("has_cover", book.hasCover);
    object.insert("identifiers", book.identifiers);
    object.insert("generated", book.identifiers.value("generated").toString());
    return object;
}

// Reading a book changes how far it was read many times an hour; that is
// what page.turned is for, and not a change to the book.
QJsonObject withoutProgress(QJsonObject book) {
    book.remove("percent_read");
    return book;
}

QJsonObject readObject(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

} // namespace

QJsonObject ScriptPlugins::Info::toJson() const {
    return {{"name", manifest.id},
            {"id", "folder:" + manifest.id},
            {"title", manifest.name},
            {"version", manifest.version},
            {"author", manifest.author},
            {"description", manifest.description},
            {"folder", manifest.folder},
            {"script", manifest.script},
            {"permissions", QJsonArray::fromStringList(manifest.permissions)},
            {"enabled", enabled},
            {"running", loaded},
            {"problem", problem},
            {"metadata", QJsonArray::fromStringList(sources)},
            {"catalogs", QJsonArray::fromStringList(catalogs)},
            {"importers", QJsonArray::fromStringList(importers)},
            {"commands", QJsonArray::fromStringList(commands)},
            {"events", QJsonArray::fromStringList(events)},
            {"panels", QJsonArray::fromStringList(panels)},
            {"formats", QJsonArray::fromStringList(formats)}};
}

QString ScriptPlugins::defaultFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omabook/plugins";
}

ScriptPlugins::ScriptPlugins(const ScriptServices &services, const QString &folder, QObject *parent, bool follow)
    : QObject(parent), m_services(services), m_folder(folder.isEmpty() ? defaultFolder() : folder), m_follow(follow) {
    if (!m_services.network)
        m_services.network = &m_network;
    m_services.reading = [this] { return m_reading; };
    // Registering is told once for a burst of it, after the script is done.
    m_announce.setSingleShot(true);
    m_announce.setInterval(0);
    connect(&m_announce, &QTimer::timeout, this, [this] {
        syncFormats();
        emit changed();
    });
    if (m_follow) {
        m_debounce.setSingleShot(true);
        m_debounce.setInterval(120);
        connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
        connect(&m_debounce, &QTimer::timeout, this, &ScriptPlugins::reload);
        // A plugin switched in the dialog, or in config.json by hand.
        if (m_services.settings)
            connect(m_services.settings, &Settings::changed, this, [this] { m_debounce.start(); });
    }
    reload();
}

ScriptPlugins::~ScriptPlugins() {
    for (const QString &id : std::as_const(m_formatIds))
        Formats::removeProvider(id);
}

void ScriptPlugins::provideFormats() {
    m_providesFormats = true;
    // At the end the window's thread waits for books still being opened
    // elsewhere; one waiting on a script here would wait on it in turn, so
    // the scripts stop answering first.
    if (QCoreApplication *app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, [this] {
            for (const auto &entry : m_plugins)
                entry.second->closeGate();
        });
    syncFormats();
}

// Formats holds what the running plugins register, no more and no less: a
// plugin loaded again brings its formats anew, one that is gone takes them.
void ScriptPlugins::syncFormats() {
    if (!m_providesFormats)
        return;
    QStringList wanted;
    for (const auto &entry : m_plugins) {
        for (const ScriptPlugin::Format &format : entry.second->formats()) {
            const FormatProvider provider = entry.second->provider(format);
            Formats::addProvider(provider);
            wanted << provider.id;
        }
    }
    for (const QString &id : std::as_const(m_formatIds))
        if (!wanted.contains(id))
            Formats::removeProvider(id);
    m_formatIds = wanted;
}

// Editors replace files rather than write into them, which drops a file from
// the watch, so everything is watched anew after every change.
void ScriptPlugins::watch() {
    if (!m_follow)
        return;
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    QString ancestor = m_folder;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList paths{ancestor, m_folder};
    for (const QFileInfo &entry : QDir(m_folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        paths << entry.filePath();
        for (const QFileInfo &file : QDir(entry.filePath()).entryInfoList({"*.json", "*.js", "*.qml"}, QDir::Files))
            paths << file.filePath();
    }
    paths.removeDuplicates();
    for (const QString &path : std::as_const(paths))
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}

// What a plugin is made of, as one value that differs when any of it does.
// The data folder and the error file are not part of it, so a plugin that
// writes its own settings is not loaded again for that.
QByteArray ScriptPlugins::stamp(const QString &folder, bool enabled) const {
    QByteArray stamp = enabled ? "on" : "off";
    for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json", "*.js", "*.qml"}, QDir::Files, QDir::Name))
        stamp += file.fileName().toUtf8() + QByteArray::number(file.lastModified().toMSecsSinceEpoch()) + QByteArray::number(file.size());
    return stamp;
}

void ScriptPlugins::report(const QString &id, const QString &problem, bool stillRunning) {
    const QString path = m_folder + "/" + id + ".error";
    if (problem.isEmpty()) {
        m_problems.remove(id);
        QFile::remove(path);
        return;
    }
    m_problems.insert(id, problem);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write((problem + (stillRunning ? "\nOmabook is using the last version that worked.\n" : "\n")).toUtf8());
}

void ScriptPlugins::adopt(std::unique_ptr<ScriptPlugin> plugin) {
    const QString id = plugin->manifest().id;
    // The one it replaces may be in the middle of a call, far up the stack.
    if (m_plugins.count(id))
        m_plugins[id].release()->deleteLater();
    m_plugins[id] = std::move(plugin);
}

void ScriptPlugins::reload() {
    watch();
    bool differs = false;
    QStringList present;
    for (const QFileInfo &entry : QDir(m_folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const ScriptManifest manifest = ScriptManifest::read(entry.filePath());
        if (!manifest.isScript())
            continue;
        const QString id = manifest.id;
        present << id;
        const bool enabled = !m_services.settings || m_services.settings->pluginEnabled("folder:" + id, true);
        const QByteArray now = stamp(entry.filePath(), enabled);
        if (m_stamps.value(id) == now)
            continue;
        m_stamps.insert(id, now);
        differs = true;
        if (!enabled) {
            m_plugins.erase(id);
            report(id, {}, false);
            continue;
        }
        auto fresh = std::make_unique<ScriptPlugin>(manifest, m_services);
        ScriptPlugin *plugin = fresh.get();
        plugin->setTimeLimit(m_limit);
        connect(plugin, &ScriptPlugin::said, this, &ScriptPlugins::said);
        connect(plugin, &ScriptPlugin::dataChanged, this, &ScriptPlugins::dataChanged);
        connect(plugin, &ScriptPlugin::touchedLibrary, this, &ScriptPlugins::touchedLibrary);
        connect(plugin, &ScriptPlugin::openBookRequested, this, &ScriptPlugins::openBookRequested);
        connect(plugin, &ScriptPlugin::graphRequested, this, &ScriptPlugins::graphRequested);
        connect(plugin, &ScriptPlugin::reloadBookRequested, this, &ScriptPlugins::reloadBookRequested);
        connect(plugin, &ScriptPlugin::askRequested, this,
                [this, plugin](int token, const QString &title, const QString &placeholder, const QString &text) {
                    if (m_attending <= 0) {
                        plugin->answerAsk(token, QJsonValue());
                        return;
                    }
                    m_questions.append({m_nextQuestion++, plugin, token, title, placeholder, text});
                    emit questionChanged();
                });
        connect(plugin, &ScriptPlugin::registered, &m_announce, qOverload<>(&QTimer::start));
        QString problem;
        if (plugin->load(&problem)) {
            // Only what goes wrong after it loaded is this version's doing.
            connect(plugin, &ScriptPlugin::failed, this, [this, id](const QString &why) {
                report(id, why, false);
                emit changed();
            });
            adopt(std::move(fresh));
            report(id, {}, false);
        } else {
            report(id, problem, m_plugins.count(id) > 0);
        }
    }
    // A plugin whose folder is gone, or is no longer a script, stops.
    for (auto it = m_plugins.begin(); it != m_plugins.end();) {
        if (present.contains(it->first)) {
            ++it;
            continue;
        }
        report(it->first, {}, false);
        m_stamps.remove(it->first);
        it = m_plugins.erase(it);
        differs = true;
    }
    for (const QString &id : m_stamps.keys())
        if (!present.contains(id))
            m_stamps.remove(id);
    syncFormats();
    if (differs) {
        // A question of a plugin that is gone has nobody left to answer.
        m_questions.erase(std::remove_if(m_questions.begin(), m_questions.end(),
                                         [this](const Question &question) {
                                             return !question.plugin || plugin(question.plugin->manifest().id) != question.plugin;
                                         }),
                          m_questions.end());
        emit changed();
        emit dataChanged();
        emit questionChanged();
    }
}

QVariantMap ScriptPlugins::question() const {
    for (const Question &question : m_questions)
        if (question.plugin)
            return {{"id", question.id}, {"plugin", question.plugin->manifest().name}, {"title", question.title},
                    {"placeholder", question.placeholder}, {"text", question.text}};
    return {};
}

void ScriptPlugins::attend(bool attending) {
    m_attending = qMax(0, m_attending + (attending ? 1 : -1));
    // The window went: what was being asked is left unanswered.
    while (m_attending == 0 && !m_questions.isEmpty())
        settle(m_questions.first().id, QJsonValue());
}

void ScriptPlugins::answer(int id, const QString &text) { settle(id, text); }
void ScriptPlugins::dismiss(int id) { settle(id, QJsonValue()); }

void ScriptPlugins::settle(int id, const QJsonValue &text) {
    for (qsizetype i = 0; i < m_questions.size(); ++i) {
        if (m_questions.at(i).id != id)
            continue;
        const Question question = m_questions.takeAt(i);
        if (question.plugin)
            question.plugin->answerAsk(question.token, text);
        emit questionChanged();
        return;
    }
}

void ScriptPlugins::setTimeLimit(std::chrono::milliseconds limit) {
    m_limit = limit;
    for (const auto &entry : m_plugins)
        entry.second->setTimeLimit(limit);
}

QList<ScriptPlugins::Info> ScriptPlugins::list() const {
    QList<Info> list;
    for (const QFileInfo &entry : QDir(m_folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        Info info;
        info.manifest = ScriptManifest::read(entry.filePath());
        if (!info.manifest.isScript())
            continue;
        info.enabled = !m_services.settings || m_services.settings->pluginEnabled("folder:" + info.manifest.id, true);
        info.problem = m_problems.value(info.manifest.id, info.manifest.problem);
        if (const ScriptPlugin *running = plugin(info.manifest.id)) {
            info.loaded = true;
            for (const ScriptPlugin::Source &source : running->sources())
                info.sources << source.name;
            for (const ScriptPlugin::Catalog &catalog : running->catalogs())
                info.catalogs << catalog.format;
            for (const ScriptPlugin::Importer &importer : running->importers())
                info.importers << importer.name;
            for (const ScriptPlugin::Command &command : running->commands())
                info.commands << command.id;
            for (const ScriptPlugin::Panel &panel : running->panels())
                info.panels << panel.place + ": " + QFileInfo(panel.source).fileName();
            for (const ScriptPlugin::Format &format : running->formats())
                info.formats << format.name + " (." + format.extensions.join(", .") + ")";
            info.events = running->events();
        }
        list << info;
    }
    return list;
}

void ScriptPlugins::retry(const QString &id) {
    m_stamps.remove(id);
    reload();
}

ScriptPlugin *ScriptPlugins::plugin(const QString &id) const {
    const auto found = m_plugins.find(id);
    return found == m_plugins.end() ? nullptr : found->second.get();
}

void ScriptPlugins::follow(const QString &event, const QJsonObject &payload) {
    if (event == "book.closed") {
        m_reading = {};
    } else if (event == "book.opened" || event == "page.turned") {
        // Where the reader is, as the events say it: the same for all three.
        m_reading = payload;
        m_reading.remove("book");
    }
    if (event.startsWith("book.")) {
        lookAtTheBook();
    }
}

void ScriptPlugins::emitEvent(const QString &event, const QJsonObject &payload) {
    follow(event, payload);
    // A handler may switch a plugin off, or change a file that loads one anew.
    QList<QPointer<ScriptPlugin>> plugins;
    for (const auto &entry : m_plugins)
        plugins << entry.second.get();
    for (const QPointer<ScriptPlugin> &plugin : std::as_const(plugins))
        if (plugin)
            plugin->emitEvent(event, payload);
}

void ScriptPlugins::libraryChanged(const QList<Book> &books, const QString &libraryPath) {
    QHash<qint64, QJsonObject> now;
    qint64 highest = 0;
    for (const Book &book : books) {
        now.insert(book.id, told(book));
        highest = qMax(highest, book.id);
    }
    // Which books this library's plugins have been told of is kept beside
    // them, so one added while Omabook wasn't running is still announced.
    const QString statePath = m_folder + "/.state.json";
    QJsonObject state = readObject(statePath);
    QJsonObject seen = state.value("seen").toObject();
    const bool known = seen.contains(libraryPath);
    const qint64 last = seen.value(libraryPath).toInteger();

    QList<QJsonObject> added;
    if (libraryPath != m_libraryPath) {
        // Another library, or the first look at this one: nothing changed,
        // but what came in since the plugins last looked is new to them.
        for (auto it = now.begin(); known && it != now.end(); ++it)
            if (it.key() > last)
                added << it.value();
    } else {
        for (auto it = now.begin(); it != now.end(); ++it) {
            const auto before = m_books.constFind(it.key());
            if (before == m_books.constEnd())
                added << it.value();
            else if (withoutProgress(*before) != withoutProgress(it.value()))
                emitEvent("book.changed", {{"book", it.value()}, {"before", *before}});
        }
        for (auto it = m_books.constBegin(); it != m_books.constEnd(); ++it)
            if (!now.contains(it.key()))
                emitEvent("book.removed", {{"id", it.key()}, {"book", it.value()}});
    }
    m_libraryPath = libraryPath;
    lookAtTheBook();
    m_books = now;
    if (!libraryPath.isEmpty() && (!known || highest > last) && QFileInfo(m_folder).isDir()) {
        seen.insert(libraryPath, highest);
        state.insert("seen", seen);
        QSaveFile file(statePath);
        file.setDirectWriteFallback(false);
        if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(state).toJson(QJsonDocument::Compact)) > 0)
            file.commit();
    }

    std::sort(added.begin(), added.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("id").toInteger() < b.value("id").toInteger();
    });
    for (const QJsonObject &book : std::as_const(added)) {
        const qint64 id = book.value("id").toInteger();
        // Importers first, so a hook on book.added sees what they made of it.
        for (const auto &entry : m_plugins) {
            for (const ScriptPlugin::Importer &importer : entry.second->importers()) {
                entry.second->imported(importer.name, book, [this, id, plugin = entry.first](const QJsonValue &details, const QString &problem) {
                    Library *library = m_services.library ? m_services.library() : nullptr;
                    const auto book = library ? library->book(id) : std::nullopt;
                    if (!problem.isEmpty()) {
                        report(plugin, problem, false);
                        emit changed();
                    }
                    if (!book || !details.isObject() || details.toObject().isEmpty())
                        return;
                    QJsonObject merged = book->meta.toJson();
                    const QJsonObject given = details.toObject();
                    for (auto it = given.begin(); it != given.end(); ++it)
                        merged.insert(it.key(), it.value());
                    const BookMeta meta = BookMeta::fromJson(merged);
                    if (!meta.title.isEmpty() && library->setMeta(id, meta))
                        emit touchedLibrary();
                });
            }
        }
        emitEvent("book.added", {{"book", book}});
    }
}

QList<MetadataSource *> ScriptPlugins::metadataSources(QObject *parent) const {
    QList<MetadataSource *> sources;
    for (const auto &entry : m_plugins)
        for (const ScriptPlugin::Source &source : entry.second->sources())
            sources << new ScriptMetadataSource(entry.second.get(), source.name, source.fields, parent);
    return sources;
}

QStringList ScriptPlugins::catalogFormats() const {
    QStringList formats;
    for (const auto &entry : m_plugins)
        for (const ScriptPlugin::Catalog &catalog : entry.second->catalogs())
            formats << catalog.format;
    return formats;
}

QString ScriptPlugins::describeCatalog(const QString &format) const {
    for (const auto &entry : m_plugins)
        for (const ScriptPlugin::Catalog &catalog : entry.second->catalogs())
            if (catalog.format == format)
                return catalog.description;
    return {};
}

QByteArray ScriptPlugins::makeCatalog(const QString &format, const QList<Book> &books, QString *problem) {
    QJsonArray list;
    for (const Book &book : books)
        list << told(book);
    for (const auto &entry : m_plugins) {
        for (const ScriptPlugin::Catalog &catalog : entry.second->catalogs()) {
            if (catalog.format != format)
                continue;
            QByteArray made;
            bool answered = false;
            QEventLoop loop;
            entry.second->makeCatalog(format, list, [&](const QJsonValue &text, const QString &why) {
                answered = true;
                *problem = why.isEmpty() && !text.isString() ? "The catalog's make answered no text." : why;
                if (problem->isEmpty())
                    made = text.toString().toUtf8();
                // An answer that came at once came before the loop began.
                loop.quit();
            });
            if (!answered) {
                QTimer::singleShot(60000, &loop, &QEventLoop::quit);
                loop.exec();
            }
            if (!answered)
                *problem = "The catalog took more than a minute and was given up on.";
            return problem->isEmpty() ? (made.isNull() ? QByteArray("") : made) : QByteArray();
        }
    }
    *problem = "There is no catalog format " + format + ".";
    return {};
}

QVariantList ScriptPlugins::commands() const {
    QVariantList list;
    for (const auto &entry : m_plugins)
        for (const ScriptPlugin::Command &command : entry.second->commands())
            list << QVariantMap{{"id", command.id}, {"title", command.title}, {"place", command.place}, {"key", command.key},
                                {"plugin", entry.second->manifest().name}, {"when", command.when},
                                {"applies", applies(entry.first, command.when, command.enabled)}};
    return list;
}

QVariantList ScriptPlugins::panels() const {
    QVariantList list;
    for (const auto &entry : m_plugins) {
        int number = 0;
        for (const ScriptPlugin::Panel &panel : entry.second->panels()) {
            // A panel that doesn't apply isn't there: it takes no room.
            const QString name = number++ == 0 ? entry.first : entry.first + "-" + QString::number(number);
            if (!applies(entry.first, panel.when, panel.visible))
                continue;
            // The file's time in the address makes an edited panel a new component.
            QUrl url = QUrl::fromLocalFile(panel.source);
            url.setQuery(QString::number(QFileInfo(panel.source).lastModified().toMSecsSinceEpoch()));
            list << QVariantMap{{"name", name}, {"place", panel.place}, {"title", panel.title}, {"width", panel.width}, {"url", url}};
        }
    }
    return list;
}

QVariantMap ScriptPlugins::data() const {
    QVariantMap data;
    for (const auto &entry : m_plugins)
        data.insert(entry.first, entry.second->data().toVariantMap());
    return data;
}

bool ScriptPlugins::applies(const QString &plugin, const QString &when, bool on) const {
    return on && (when != "generated" || m_maker == plugin);
}

void ScriptPlugins::setSelectedBook(qint64 id) {
    if (id == m_selectedBook)
        return;
    m_selectedBook = id;
    emit selectedBookChanged();
    lookAtTheBook();
}

// Whose book is in front of the reader: the open one, else the selected.
void ScriptPlugins::lookAtTheBook() {
    const qint64 id = m_reading.isEmpty() ? m_selectedBook : m_reading.value("id").toInteger();
    const Library *library = m_services.library ? m_services.library() : nullptr;
    const auto book = library && id > 0 ? library->book(id) : std::nullopt;
    const QString maker = book ? book->identifiers.value("generated").toString() : QString();
    if (maker == m_maker)
        return;
    m_maker = maker;
    emit changed();
}

void ScriptPlugins::run(const QString &command, qint64 book) {
    // Whole, it is one command; by plugin and id, one too; the id alone
    // may be several plugins', and then nobody's is run.
    QList<QPair<ScriptPlugin *, ScriptPlugin::Command>> found;
    for (int exactness = 0; exactness < 3 && found.isEmpty(); ++exactness) {
        for (const auto &entry : m_plugins) {
            for (const ScriptPlugin::Command &known : entry.second->commands()) {
                const QString named = exactness == 0 ? known.id : exactness == 1 ? known.id.section('.', 1) : known.id.section(':', -1);
                if (named == command)
                    found.append({entry.second.get(), known});
            }
        }
    }
    if (found.size() == 1)
        found.first().first->runCommand(found.first().second.id, {{"book", book}, {"place", found.first().second.place}});
    else if (found.size() > 1)
        emit said(QString("Several plugins have a command \"%1\": name the plugin too, as in \"%2\".")
                      .arg(command, found.first().second.id.section('.', 1)));
}

int ScriptPlugins::request(const QString &plugin, const QString &function, const QVariantList &arguments) {
    const int id = m_nextRequest++;
    ScriptPlugin *running = this->plugin(plugin);
    // Answered through the event loop even when there is nothing to ask,
    // so whoever asked has the number before the answer.
    if (!running) {
        QTimer::singleShot(0, this, [this, id, plugin] { emit answered(id, {}, "There is no plugin " + plugin + " running."); });
        return id;
    }
    QPointer<ScriptPlugins> self(this);
    const auto returned = std::make_shared<bool>(false);
    running->callFunction(function, QJsonArray::fromVariantList(arguments), [self, id, returned](const QJsonValue &result, const QString &problem) {
        if (!self)
            return;
        if (*returned)
            emit self->answered(id, result.toVariant(), problem);
        else
            QTimer::singleShot(0, self, [self, id, result, problem] { emit self->answered(id, result.toVariant(), problem); });
    });
    *returned = true;
    return id;
}

QVariant ScriptPlugins::call(const QString &plugin, const QString &function, const QVariantList &arguments) {
    ScriptPlugin *running = this->plugin(plugin);
    if (!running)
        return {};
    QString problem;
    const QJsonValue answer = running->callNow(function, QJsonArray::fromVariantList(arguments), &problem);
    if (!problem.isEmpty()) {
        report(plugin, problem, false);
        emit changed();
    }
    return answer.toVariant();
}

bool ScriptPlugins::wait(int milliseconds) {
    return waitFor([this] {
        return std::all_of(m_plugins.begin(), m_plugins.end(), [](const auto &entry) { return entry.second->idle(); });
    }, milliseconds);
}

bool ScriptPlugins::waitFor(const std::function<bool()> &idle, int milliseconds) {
    QElapsedTimer spent;
    spent.start();
    // A promise's next step is itself an event, so a script is done when it
    // has stayed idle for a few turns of the loop, not the moment it is.
    int quiet = 0;
    while (spent.elapsed() < milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(quiet > 0 ? 0 : 5, &loop, &QEventLoop::quit);
        loop.exec();
        quiet = idle() ? quiet + 1 : 0;
        if (quiet >= 4)
            return true;
    }
    return false;
}

CommandScripts::CommandScripts(Settings *settings, const QString &libraryRoot, const QString &folder)
    : m_library(libraryRoot), m_opened(m_library.open()) {
    ScriptServices services;
    services.settings = settings;
    services.library = [this] { return m_opened ? &m_library : nullptr; };
    services.ai = [settings](QString *why) { return makeAiProvider(*settings, why); };
    m_plugins = std::make_unique<ScriptPlugins>(services, folder, nullptr, false);
}

namespace {

// The script plugins of a program with no window, kept for as long as the
// application is.
class OnDemandScripts : public QObject {
public:
    explicit OnDemandScripts(QObject *parent) : QObject(parent) {
        ScriptServices services;
        services.settings = &m_settings;
        // Opened only when a script asks for it: opening makes a library
        // where there is none, which looking at a file shouldn't.
        services.library = [this]() -> Library * {
            if (!m_library) {
                m_library = std::make_unique<Library>(m_settings.libraryPath());
                m_opened = m_library->open();
            }
            return m_opened ? m_library.get() : nullptr;
        };
        services.ai = [this](QString *why) { return makeAiProvider(m_settings, why); };
        m_plugins = std::make_unique<ScriptPlugins>(services, QString(), nullptr, false);
        m_plugins->provideFormats();
        m_looked.start();
    }

    // A command that runs for long (the MCP server) has no event loop to be
    // told of a changed file by, so the folder is looked at again when
    // asked, and not more than once a second.
    void lookAgain() {
        if (m_looked.elapsed() < 1000)
            return;
        m_looked.restart();
        m_plugins->reload();
    }

private:
    Settings m_settings;
    std::unique_ptr<Library> m_library;
    bool m_opened = false;
    std::unique_ptr<ScriptPlugins> m_plugins;
    QElapsedTimer m_looked;
};

QPointer<OnDemandScripts> onDemand;

} // namespace

void ScriptFormats::loadOnDemand() {
    Formats::setProviderLoader([] {
        // The scripts live on the application's thread, and there only.
        QCoreApplication *app = QCoreApplication::instance();
        if (!app || QThread::currentThread() != app->thread())
            return;
        if (onDemand)
            onDemand->lookAgain();
        else
            onDemand = new OnDemandScripts(app);
    });
}

void ScriptFormats::unload() {
    Formats::setProviderLoader({});
    delete onDemand;
}

ScriptMetadataSource::ScriptMetadataSource(ScriptPlugin *plugin, const QString &name, const QStringList &fields, QObject *parent)
    : MetadataSource(parent), m_plugin(plugin), m_name(name), m_fields(fields) {}

void ScriptMetadataSource::search(const BookMeta &book) {
    // Answered through the event loop even when the script answers at once
    // or isn't there: whoever asked is still connecting.
    auto answer = [this](const QList<MetaCandidate> &candidates, const QString &problem) {
        QTimer::singleShot(0, this, [this, candidates, problem] {
            if (problem.isEmpty())
                emit found(candidates);
            else
                emit failed(m_name + ": " + problem);
        });
    };
    if (!m_plugin) {
        answer({}, "The plugin was switched off or changed while it was being asked.");
        return;
    }
    m_plugin->search(m_name, book.toJson(), [this, guard = QPointer<ScriptMetadataSource>(this), answer](const QJsonValue &result, const QString &problem) {
        if (!guard)
            return;
        QList<MetaCandidate> candidates;
        for (const QJsonValue &value : result.toArray()) {
            const MetaCandidate candidate = MetaCandidate::fromJson(value.toObject(), m_name);
            if (!candidate.meta.title.isEmpty())
                candidates << candidate;
        }
        answer(candidates, problem.isEmpty() && !result.isArray() && !result.isNull() ? "search should answer a list of candidates."
                                                                                        : problem);
    });
}
