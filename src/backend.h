#pragma once

#include <QFileSystemWatcher>
#include <QFuture>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

#include "ai.h"
#include "booklistmodel.h"
#include "filepicker.h"
#include "library.h"
#include "metadatasource.h"
#include "graphjobs.h"
#include "pluginmanager.h"
#include "plugins.h"
#include "scriptplugins.h"
#include "settings.h"

// The library as the window uses it: the shelf, adding and removing books,
// editing their details, and looking details up online or with AI. It holds
// no book knowledge of its own; that is Library's and the format readers'.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(BookListModel *books READ books CONSTANT)
    Q_PROPERTY(QString libraryPath READ libraryPath NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryName READ libraryName NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryError READ libraryError NOTIFY libraryChanged)
    // Bumped whenever the books change, for bindings that read book().
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY busyChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY candidatesChanged)
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY candidatesChanged)
    Q_PROPERTY(QString searchProblem READ searchProblem NOTIFY candidatesChanged)
    // Each source asked in the running look-up: its name, what it can bring,
    // and whether it is still being asked, found something, or failed.
    Q_PROPERTY(QVariantList lookUpSources READ lookUpSources NOTIFY candidatesChanged)
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    // The script plugins, for the window: their commands and panels, what
    // each set for its panels (`data`), and `run` and `call` into them.
    Q_PROPERTY(ScriptPlugins *scripts READ scripts CONSTANT)
    // The plugin manager: every plugin sorted by what it does, what needs
    // attention, and installing, removing, ordering and trying them.
    Q_PROPERTY(PluginManager *pluginManager READ pluginManager CONSTANT)
    // Building the knowledge graph for books after the fact.
    Q_PROPERTY(GraphJobs *graphJobs READ graphJobs CONSTANT)
    // The Calibre plugin as the window shows it: whether the user switched
    // it off, the Calibre libraries found on this computer (each with its
    // path, name, book count and the answer given when it was offered), and
    // the one being offered for import now, empty when there is none.
    Q_PROPERTY(bool calibreSwitchedOff READ calibreSwitchedOff NOTIFY calibreChanged)
    Q_PROPERTY(QVariantList calibreLibraries READ calibreLibraries NOTIFY calibreChanged)
    Q_PROPERTY(QVariantMap calibreOffer READ calibreOffer NOTIFY calibreChanged)
    // How this library is looked at: "covers", "spines", "carousel", "list"
    // or "stacks", and what the stacks are piled by ("series", "author",
    // "tag" or "status"). Remembered per library, with the shelf's filters.
    Q_PROPERTY(QString view READ view WRITE setView NOTIFY shelfChanged)
    Q_PROPERTY(QString group READ group WRITE setGroup NOTIFY shelfChanged)

public:
    using AiFactory = std::function<std::unique_ptr<AiProvider>(QString *why)>;
    // The catalogs a look-up asks, each owned by the parent it is given.
    using Catalogs = std::function<QList<MetadataSource *>(QObject *parent)>;
    // The ssh program and its first arguments; tests use a stand-in.
    void setSsh(const QStringList &ssh) { m_ssh = ssh; }

    // Tests pass their own picker and AI; without an AI factory the settings decide.
    Backend(Settings *settings, FilePicker *picker, QObject *parent = nullptr, AiFactory ai = {}, Catalogs catalogs = {},
            const QString &pluginFolder = {}, const QString &calibreDebug = {});
    ~Backend() override;

    BookListModel *books() { return &m_model; }
    Library *library() { return m_library.get(); }
    QString libraryPath() const { return m_library ? m_library->root() : QString(); }
    QString libraryName() const;
    QString libraryError() const { return m_libraryError; }
    int revision() const { return m_revision; }
    QString view() const { return m_view; }
    void setView(const QString &view);
    QString group() const { return m_group; }
    void setGroup(const QString &group);
    static QStringList views() { return {"covers", "spines", "carousel", "list", "stacks"}; }
    static QStringList groupings() { return {"series", "author", "tag", "status"}; }
    bool busy() const { return m_jobs > 0; }
    QString status() const { return m_status; }
    QString message() const { return m_message; }
    bool searching() const { return m_pendingSources > 0; }
    QVariantList candidates() const;
    QString searchProblem() const { return m_searchProblem; }
    QVariantList lookUpSources() const;
    QVariantList plugins() const;
    Q_INVOKABLE void setPluginEnabled(const QString &id, bool enabled);
    // Asks Calibre again which plugins it has, beside the window.
    Q_INVOKABLE void refreshPlugins();

    bool calibreSwitchedOff() const { return m_calibreSwitchedOff; }
    QVariantList calibreLibraries() const { return m_calibreLibraries; }
    QVariantMap calibreOffer() const { return m_calibreOffers.value(0).toMap(); }
    // Looks for Calibre libraries because the user asked: offers every one
    // not imported or refused for good, and says so when there is none.
    Q_INVOKABLE void findCalibreLibraries();
    // The answer to the library on offer: "import", "later" or "never".
    Q_INVOKABLE void answerCalibreOffer(const QString &answer);
    // Imports a Calibre library, whatever was answered before.
    Q_INVOKABLE void importCalibreLibrary(const QString &path);
    Plugins *pluginRegistry() { return &m_plugins; }
    // The script plugins. Backend tells them what happens to the library;
    // the reader tells them what happens to the open book.
    ScriptPlugins *scripts() { return m_scripts.get(); }
    PluginManager *pluginManager() { return m_pluginManager.get(); }
    GraphJobs *graphJobs() { return m_graphJobs; }

    // For what works on the whole library (the librarian): a model, the
    // sources a look-up asks, a book's opening pages, and a cover fetched.
    std::unique_ptr<AiProvider> newAi(QString *why) const { return m_aiFactory(why); }
    QList<MetadataSource *> newSources(QObject *parent) { return m_catalogs(parent); }
    QString openingPagesOf(qint64 id) const { return openingPages(id); }
    void fetchCoverFor(qint64 id, const QUrl &url);

    // Everything the window shows about one book; empty when there is none.
    Q_INVOKABLE QVariantMap book(qint64 id) const;

    Q_INVOKABLE void addBooks();
    Q_INVOKABLE void addFolder();
    // Files and folders dropped on the window or named on the command line.
    Q_INVOKABLE void addUrls(const QList<QUrl> &urls);
    Q_INVOKABLE void remove(qint64 id);
    Q_INVOKABLE void saveDetails(qint64 id, const QVariantMap &details);
    // What is said of a book's reading and how it is held: {shelf, rating,
    // finished, paper, hasFile}.
    Q_INVOKABLE QVariantMap reading(qint64 id) const;
    // Adds a book that has no file, one on a real shelf or one to read some
    // day, from the same details saveDetails takes; 0 when it has no title.
    Q_INVOKABLE qint64 newEntry(const QVariantMap &details);
    Q_INVOKABLE void chooseCover(qint64 id);
    Q_INVOKABLE void exportBook(qint64 id);

    // Asks the catalogs, and the AI when there is one, about a book.
    Q_INVOKABLE void lookUp(qint64 id);
    Q_INVOKABLE void applyCandidate(int index);
    Q_INVOKABLE void stopLookUp();

    // Brings this library and the computer named in the settings in step.
    Q_INVOKABLE void sync();
    Q_INVOKABLE void chooseLibrary();
    Q_INVOKABLE void useLibrary(const QString &path);
    // Takes a library off the list; with `trash` its folder goes to the
    // trash too. What went wrong, if anything, is said in the window.
    Q_INVOKABLE void forgetLibrary(const QString &path, bool trash = false);
    Q_INVOKABLE void dismissMessage();
    Q_INVOKABLE void say(const QString &message);
    // Asks for a reading site's export file to read, or where to write
    // one; the answer is readingFilePicked.
    void pickReadingFile(bool save, const QString &suggested = {});

public slots:
    void reload();

signals:
    void libraryChanged();
    void revisionChanged();
    void busyChanged();
    void messageChanged();
    void candidatesChanged();
    void pluginsChanged();
    void calibreChanged();
    void shelfChanged();
    // A book was added by the user's own doing, so the shelf can select it.
    void bookAdded(qint64 id);
    void readingFilePicked(bool save, const QString &file);

private:
    void openLibrary(const QString &path);
    // Reads how the library's shelf was left from the settings, and writes it there.
    void takeShelf();
    void keepShelf();
    void watchLibrary();
    void picked(FilePicker::Purpose purpose, const QList<QUrl> &urls);
    void importFinished(const ImportSummary &summary, bool calibreLeftOut);
    void loadPlugins(bool refresh);
    void lookForCalibre(bool asked);
    void setStatus(const QString &status);
    void sourceAnswered(int index, const QList<MetaCandidate> &candidates, const QString &problem);
    void askAiWhenCatalogsAreDone();
    QString openingPages(qint64 id) const;

    Settings *m_settings;
    FilePicker *m_picker;
    AiFactory m_aiFactory;
    Catalogs m_catalogs;
    QStringList m_ssh;
    std::unique_ptr<Library> m_library;
    QString m_libraryError;
    BookListModel m_model;
    QString m_view = "covers";
    QString m_group = "series";
    // Set while the shelf is read from the settings or written to them, so
    // one doesn't answer the other.
    bool m_shelfMoving = false;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadSoon;
    int m_revision = 0;
    int m_jobs = 0;
    QString m_status;
    QString m_message;
    qint64 m_pickingFor = 0;

    QNetworkAccessManager m_network;
    std::unique_ptr<AiProvider> m_lookUpAi;
    struct SourceState {
        QString name;
        QStringList fields;
        bool asking = true;
        int found = 0;
        QString problem;
    };
    Plugins m_plugins;
    std::unique_ptr<ScriptPlugins> m_scripts;
    std::unique_ptr<PluginManager> m_pluginManager;
    GraphJobs *m_graphJobs = nullptr; // a child of this, so it goes with it
    QFuture<void> m_pluginsLoading;
    QFuture<void> m_calibreLooking;
    bool m_calibreSwitchedOff = false;
    QVariantList m_calibreLibraries;
    QVariantList m_calibreOffers;
    QList<MetadataSource *> m_sources;
    QList<SourceState> m_sourceStates;
    AiMetadataSource *m_aiSource = nullptr;
    bool m_aiAsked = false;
    QList<MetaCandidate> m_candidates;
    int m_pendingSources = 0;
    qint64 m_lookUpId = 0;
    QString m_searchProblem;
};
