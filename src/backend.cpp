#include "backend.h"

#include "graph.h"

#include "libraries.h"
#include "log.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoopLocker>
#include <QFileInfo>
#include <QImageReader>
#include <QPointer>
#include <QThreadPool>
#include <QtConcurrent>
#include <algorithm>

#include "graphmodel.h"
#include "importer.h"
#include "sync.h"

Backend::Backend(Settings *settings, FilePicker *picker, QObject *parent, AiFactory ai, Catalogs catalogs,
                 const QString &pluginFolder, const QString &calibreDebug)
    : QObject(parent), m_settings(settings), m_picker(picker), m_aiFactory(std::move(ai)), m_catalogs(std::move(catalogs)),
      m_plugins(settings, pluginFolder, calibreDebug) {
    // The graph views are QML types of the app's own, known before any window loads.
    registerGraphTypes();
    if (!m_aiFactory)
        m_aiFactory = [settings](QString *why) { return makeAiProvider(*settings, why); };
    if (!m_catalogs)
        m_catalogs = [this](QObject *parent) {
            return m_plugins.sources(m_plugins.known(), &m_network, parent)
                   + (m_scripts ? m_scripts->metadataSources(parent) : QList<MetadataSource *>());
        };
    // Asking Calibre which plugins it has takes a moment, so it happens
    // beside the window; the list fills in when it answers.
    loadPlugins(false);
    // So does looking for Calibre's libraries; one that was never offered
    // is offered once the window is up.
    lookForCalibre(false);
    m_model.setSort(m_settings->sort());
    m_model.setReversed(m_settings->sortReversed());
    connect(&m_model, &BookListModel::sortChanged, this, [this] {
        if (m_shelfMoving)
            return;
        // A collection keeps its own order; the library's is in the settings.
        if (m_collection) {
            keepCollectionShelf();
            return;
        }
        m_shelfMoving = true;
        m_settings->setSort(m_model.sort());
        m_settings->setSortReversed(m_model.reversed());
        m_shelfMoving = false;
    });
    connect(&m_model, &BookListModel::filtersChanged, this, &Backend::keepShelf);
    connect(&m_model, &BookListModel::knowledgeWanted, this, &Backend::buildKnowledge);
    m_knowSoon.setSingleShot(true);
    connect(&m_knowSoon, &QTimer::timeout, this, &Backend::readKnowledge);
    // config.json is followed while the app runs, the shelf's part of it too.
    connect(m_settings, &Settings::changed, this, [this] {
        if (m_shelfMoving)
            return;
        takeShelf();
    });

    // A book added from the command line, or by an agent, shows up without a
    // restart: the database's files change, and the shelf follows.
    m_reloadSoon.setSingleShot(true);
    m_reloadSoon.setInterval(250);
    connect(&m_reloadSoon, &QTimer::timeout, this, &Backend::reloadIfChanged);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reloadSoon.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadSoon.start(); });

    connect(m_picker, &FilePicker::picked, this, &Backend::picked);
    connect(m_picker, &FilePicker::failed, this, &Backend::say);
    openLibrary(m_settings->libraryPath());

    startScripts(pluginFolder);
    m_graphJobs = new GraphJobs(this);
}

Backend::~Backend() {
    m_pluginsLoading.waitForFinished();
    m_calibreLooking.waitForFinished();
}

void Backend::loadPlugins(bool refresh) {
    m_pluginsLoading.waitForFinished();
    m_pluginsLoading = QtConcurrent::run([this, refresh] {
        m_plugins.list(refresh);
        QMetaObject::invokeMethod(this, &Backend::pluginsChanged, Qt::QueuedConnection);
    });
}

void Backend::refreshPlugins() { loadPlugins(true); }

// Calibre's libraries are found beside the window: each one's database is
// opened to count its books. Nothing is looked for while the plugin is off.
void Backend::lookForCalibre(bool asked) {
    m_calibreLooking.waitForFinished();
    m_calibreLooking = QtConcurrent::run([this, asked] {
        const CalibrePlugin &calibre = m_plugins.calibre();
        const bool switchedOff = calibre.switchedOff();
        QVariantList libraries, offers;
        for (const CalibreLibrary &library : calibre.libraries())
            libraries << library.toJson().toVariantMap();
        for (const CalibreLibrary &library : calibre.librariesToOffer(asked))
            offers << library.toJson().toVariantMap();
        QMetaObject::invokeMethod(
            this,
            [this, asked, switchedOff, libraries, offers] {
                m_calibreSwitchedOff = switchedOff;
                m_calibreLibraries = libraries;
                m_calibreOffers = offers;
                emit calibreChanged();
                if (asked && switchedOff)
                    say(tr("The Calibre plugin is switched off, so Calibre libraries aren't looked for. Switch it on under Plugins."));
                else if (asked && offers.isEmpty())
                    say(libraries.isEmpty() ? tr("No Calibre library was found on this computer.")
                                            : tr("Every Calibre library found here was already imported or set aside."));
            },
            Qt::QueuedConnection);
    });
}

void Backend::findCalibreLibraries() { lookForCalibre(true); }

void Backend::answerCalibreOffer(const QString &answer) {
    if (m_calibreOffers.isEmpty())
        return;
    const QString path = m_calibreOffers.takeFirst().toMap().value("path").toString();
    emit calibreChanged();
    if (answer == "import") {
        importCalibreLibrary(path);
    } else if (answer == "later" || answer == "never") {
        m_plugins.calibre().answer(path, answer);
        lookForCalibre(false);
    }
}

void Backend::importCalibreLibrary(const QString &path) { addUrls({QUrl::fromLocalFile(path)}); }

QString Backend::libraryName() const { return QFileInfo(libraryPath()).fileName(); }

void Backend::openLibrary(const QString &path) {
    auto library = std::make_unique<Library>(path);
    QString error;
    if (library->open(&error)) {
        m_library = std::move(library);
        m_libraryError.clear();
    } else {
        m_library.reset();
        m_libraryError = error;
    }
    emit libraryChanged();
    m_fileManager.follow(m_library ? m_library->root() : QString());
    // What was known of the other library's books is no use here.
    m_model.setKnowledge({});
    takeShelf();
    reload();
}

void Backend::takeShelf() {
    if (m_shelfMoving)
        return;
    const QJsonObject shelf = m_settings->shelf(m_settings->libraryPath());
    m_libraryView = views().contains(shelf.value("view").toString()) ? shelf.value("view").toString() : "covers";
    m_libraryGroup = groupings().contains(shelf.value("group").toString()) ? shelf.value("group").toString() : "series";
    m_shelfMoving = true;
    m_model.setFilters(shelf.value("filters").toObject().toVariantMap());
    m_shelfMoving = false;
    // The collection that was in view, while it is still there.
    const qint64 collection = shelf.value("collection").toInteger();
    const qint64 before = std::exchange(m_collection, m_library && collection > 0 && m_library->collection(collection) ? collection : 0);
    if (m_collection != before) {
        if (m_library)
            readCollections(m_library->books());
        emit collectionsChanged();
    }
    showCollectionShelf();
}

void Backend::keepShelf() {
    if (m_shelfMoving)
        return;
    m_shelfMoving = true;
    m_settings->setShelf(m_settings->libraryPath(), {{"view", m_libraryView}, {"group", m_libraryGroup},
                                                     {"filters", QJsonObject::fromVariantMap(m_model.filters())},
                                                     {"collection", m_collection}});
    m_shelfMoving = false;
}

void Backend::setView(const QString &view) {
    if (view == m_view || !views().contains(view))
        return;
    m_view = view;
    emit shelfChanged();
    if (m_collection) {
        keepCollectionShelf();
        return;
    }
    m_libraryView = view;
    keepShelf();
}

void Backend::setGroup(const QString &group) {
    if (group == m_group || !groupings().contains(group))
        return;
    m_group = group;
    emit shelfChanged();
    if (m_collection) {
        keepCollectionShelf();
        return;
    }
    m_libraryGroup = group;
    keepShelf();
}

// The database is replaced or rewritten through its journal, which drops a
// file from the watch, so the watch is set again after every change.
void Backend::watchLibrary() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (!m_library)
        return;
    for (const QString &path : {m_library->root(), m_library->databasePath(), m_library->databasePath() + "-wal"})
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}

// The library's files changed on disk: by this window, a command, an agent
// or a plugin. The database counts what changed, so a write to the knowledge
// graph (constant while a book is read or a story written) costs a look at
// two numbers, not a reading of every book.
void Backend::reloadIfChanged() {
    if (!m_library) {
        reload();
        return;
    }
    const Library::Changes now = m_library->changes();
    const Library::Changes before = std::exchange(m_changes, now);
    if (now.format != before.format)
        emit bookFormatsChanged();
    if (now.books != before.books) {
        reload();
        return;
    }
    watchLibrary();
    if (now.graph != before.graph) {
        GraphFeed::announce();
        if (m_model.searching())
            buildKnowledge();
    }
}

void Backend::buildKnowledge() {
    if (!m_library || m_settings->graph() == "off") {
        if (m_model.knowledge())
            m_model.setKnowledge({});
        return;
    }
    if (m_knowing) {
        m_knowAgain = true;
        return;
    }
    const auto &known = m_model.knowledge();
    if (!known) {
        readKnowledge();
        return;
    }
    if (known->changes == m_library->changes() || m_knowSoon.isActive())
        return;
    m_knowSoon.start(int(std::clamp<qint64>(1000 + 4 * m_knowingTook, 1000, 30000)));
}

void Backend::readKnowledge() {
    if (!m_library || m_knowing)
        return;
    m_knowing = true;
    const QString root = m_library->root();
    const QPointer<Backend> self(this);
    QThreadPool::globalInstance()->start([self, root, before = m_model.knowledge()] {
        QElapsedTimer took;
        took.start();
        Library own(root);
        const auto index = std::make_shared<const KnowledgeIndex>(own.open() ? KnowledgeIndex::since(own, before.get()) : KnowledgeIndex());
        QMetaObject::invokeMethod(qApp, [self, root, index, took = took.elapsed()] {
            if (!self)
                return;
            self->m_knowing = false;
            self->m_knowingTook = took;
            // Read for a library that is no longer the one in use.
            if (self->m_library && self->m_library->root() == root)
                self->m_model.setKnowledge(index);
            if (std::exchange(self->m_knowAgain, false))
                self->buildKnowledge();
        });
    });
}

void Backend::reload() {
    if (m_library)
        m_changes = m_library->changes();
    const QList<Book> books = m_library ? m_library->books() : QList<Book>();
    readCollections(books);
    m_model.setBooks(books, libraryPath());
    watchLibrary();
    m_fileManager.libraryChanged();
    ++m_revision;
    emit revisionChanged();
    // The scripts hear of every book that came, went or changed, whoever did it.
    if (m_scripts && m_library)
        m_scripts->libraryChanged(m_library->books(), libraryPath());
}

QVariantMap Backend::book(qint64 id) const {
    // A plugin's launcher, which is on the shelf and in no library.
    if (id < 0) {
        const QVariantMap launcher = m_scripts ? m_scripts->launcher(id) : QVariantMap();
        return launcher.isEmpty() ? QVariantMap() : launcherToMap(launcher);
    }
    const auto book = m_library ? m_library->book(id) : std::nullopt;
    return book ? bookToMap(*book, m_library->root()) : QVariantMap();
}

void Backend::say(const QString &message) {
    if (!message.isEmpty())
        qCInfo(lcApp).noquote() << "said:" << message;
    m_message = message;
    emit messageChanged();
}

void Backend::dismissMessage() { say({}); }

void Backend::forgetLibrary(const QString &path, bool trash) {
    const QString before = libraryPath();
    QString error;
    if (!(trash ? Libraries::trash(*m_settings, path, &error) : Libraries::forget(*m_settings, path, &error))) {
        say(error);
        return;
    }
    // Forgetting the library in use moved on to another, which is opened.
    if (libraryPath() != before) {
        m_model.setQuery({});
        openLibrary(libraryPath());
        m_scripts->emitEvent("library.switched", {{"path", libraryPath()}, {"name", libraryName()}});
    }
}

void Backend::useLibrary(const QString &path) {
    if (path.isEmpty() || QDir(path).absolutePath() == libraryPath())
        return;
    // A Calibre library is Calibre's to keep; its books are brought into the
    // library in use rather than Omabook moving in beside Calibre.
    if (m_library && Libraries::isCalibreOnly(path) && m_plugins.calibre().enabled()) {
        say(tr("%1 is a Calibre library, so its books are being imported into %2 rather than opened there.")
                .arg(QFileInfo(path).fileName(), libraryName()));
        importCalibreLibrary(path);
        return;
    }
    m_settings->setLibraryPath(path);
    m_model.setQuery({});
    openLibrary(path);
    if (!m_libraryError.isEmpty())
        say(m_libraryError);
    m_scripts->emitEvent("library.switched", {{"path", libraryPath()}, {"name", libraryName()}});
}

void Backend::addUrls(const QList<QUrl> &urls) {
    if (!m_library || urls.isEmpty())
        return;
    QStringList paths;
    for (const QUrl &url : urls)
        paths << (url.isLocalFile() ? url.toLocalFile() : url.toString());
    ++m_jobs;
    setStatus(tr("Adding books…"));

    // Hashing and copying a shelf of books takes a while, so it happens on a
    // worker with its own connection to the library. Closing the window
    // meanwhile leaves the app running until the last book is in.
    const QString root = m_library->root();
    // Whether a Calibre library is read as one is the Calibre plugin's to
    // say; off, it is a folder of books, and the summary says so.
    const CalibrePlugin &calibre = m_plugins.calibre();
    const ImportOptions options = calibre.importOptions();
    const bool calibreLeftOut = !options.calibre && std::ranges::any_of(paths, [](const QString &path) {
        return QFileInfo(path + "/metadata.db").isFile();
    });
    QPointer<Backend> self(this);
    QThreadPool::globalInstance()->start([self, root, paths, options, calibreLeftOut] {
        QEventLoopLocker locker;
        ImportSummary summary;
        Library library(root);
        QString error;
        if (!library.open(&error)) {
            summary.problems << error;
        } else {
            summary = importBooks(library, paths, [self](int done, int total) {
                QMetaObject::invokeMethod(
                    qApp, [self, done, total] { if (self) self->setStatus(Backend::tr("Adding %1 of %2…").arg(done).arg(total)); },
                    Qt::QueuedConnection);
            }, options);
        }
        QMetaObject::invokeMethod(
            qApp, [self, summary, calibreLeftOut] { if (self) self->importFinished(summary, calibreLeftOut); }, Qt::QueuedConnection);
    });
}

void Backend::setStatus(const QString &status) {
    m_status = status;
    emit busyChanged();
}

void Backend::importFinished(const ImportSummary &summary, bool calibreLeftOut) {
    // A file that joined a book here without one was added, to the window.
    const int added = summary.added + summary.joined, duplicates = summary.duplicates;
    const QStringList &problems = summary.problems;
    const qint64 lastId = summary.lastId;
    --m_jobs;
    setStatus({});
    QList<qint64> came;
    for (const ImportSummary::Item &item : summary.items)
        if (item.id > 0 && !item.duplicate)
            came << item.id;
    putInShownCollection(came);
    reload();
    QStringList lines;
    if (added > 0)
        lines << tr("%Ln books added", "", added);
    if (duplicates > 0)
        lines << tr("%Ln were already in the library", "", duplicates);
    // What Calibre knew of the reading, which came along.
    if (summary.read > 0)
        lines << tr("%Ln are read, by Calibre's account", "", summary.read);
    if (summary.reading > 0)
        lines << tr("%Ln are being read", "", summary.reading);
    if (summary.updated > 0)
        lines << tr("%Ln already here were brought up to date", "", summary.updated);
    if (calibreLeftOut)
        lines << tr("The Calibre plugin is switched off, so the Calibre library was added as a plain folder of books.");
    if (added == 0 && duplicates == 0 && problems.isEmpty())
        lines << tr("No books were found there.");
    // The first few reasons say enough; a folder of odd files could give hundreds.
    lines << problems.mid(0, 3);
    if (problems.size() > 3)
        lines << tr("%Ln more could not be added", "", int(problems.size()) - 3);
    say(lines.join("\n"));
    if (lastId && added + duplicates == 1)
        emit bookAdded(lastId);
    // A Calibre library that came in needs no offering any more.
    if (!summary.calibreLibraries.isEmpty()) {
        for (const QString &library : summary.calibreLibraries)
            m_plugins.calibre().answer(library, "imported");
        lookForCalibre(false);
    }
}

void Backend::remove(qint64 id) {
    if (m_library)
        if (const auto removed = m_library->remove(id); !removed)
            say(removed.error());
    reload();
}

QVariantMap Backend::reading(qint64 id) const {
    const auto book = m_library ? m_library->book(id) : std::nullopt;
    if (!book)
        return {};
    return {{"shelf", book->shelf}, {"rating", book->rating}, {"finished", book->finished}, {"paper", book->paper},
            {"hasFile", !book->formats.isEmpty()}};
}

qint64 Backend::newEntry(const QVariantMap &details) {
    BookMeta meta;
    meta.title = details.value("title").toString().simplified();
    if (!m_library || meta.title.isEmpty())
        return 0;
    const Library::Added added = m_library->addEntry(meta, details.value("paper").toBool());
    if (!added.error.isEmpty()) {
        say(added.error);
        return 0;
    }
    putInShownCollection({added.id});
    // The rest goes in the way an edit does.
    saveDetails(added.id, details);
    return added.id;
}

QVariantList Backend::plugins() const {
    QVariantList list;
    for (const PluginInfo &plugin : m_plugins.known())
        list << QVariantMap{{"id", plugin.id}, {"name", plugin.name}, {"kind", plugin.kind}, {"origin", plugin.origin},
                            {"version", plugin.version}, {"author", plugin.author}, {"description", plugin.description},
                            {"fields", plugin.fields}, {"enabled", plugin.enabled}, {"problem", plugin.problem},
                            {"switchable", plugin.switchable}, {"permissions", plugin.permissions}};
    return list;
}

void Backend::setPluginEnabled(const QString &id, bool enabled) {
    if (!m_plugins.setEnabled(id, enabled))
        return;
    emit pluginsChanged();
    // Switching Calibre changes what is found: its plugins and libraries
    // appear when it goes on, and are gone when it goes off.
    if (id == CalibrePlugin::id) {
        loadPlugins(false);
        lookForCalibre(false);
    }
}

void Backend::stopLookUp() {
    // The sources go first: one of them listens to the AI.
    for (MetadataSource *source : std::as_const(m_sources)) {
        source->disconnect(this);
        delete source;
    }
    m_sources.clear();
    m_sourceStates.clear();
    m_aiSource = nullptr;
    m_aiAsked = false;
    if (m_lookUpAi)
        m_lookUpAi->cancel();
    m_lookUpAi.reset();
    m_candidates.clear();
    m_pendingSources = 0;
    m_searchProblem.clear();
    emit candidatesChanged();
}
