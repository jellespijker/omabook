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
#include "audiodevices.h"
#include "booklistmodel.h"
#include "dictation.h"
#include "spokenanswer.h"
#include "voicecommands.h"
#include "filepicker.h"
#include "folio.h"
#include "library.h"
#include "metadatasource.h"
#include "graphjobs.h"
#include "plugindesk.h"
#include "pluginmanager.h"
#include "plugins.h"
#include "scriptplugins.h"
#include "settings.h"
#include "spacedesk.h"
#include "speechinstall.h"
#include "transcriptdesk.h"

struct SyncSummary;

// The library as the window uses it: the shelf, adding and removing books,
// editing their details, and looking details up online or with AI. It holds
// no book knowledge of its own; that is Library's and the format readers'.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(BookListModel *books READ books CONSTANT)
    Q_PROPERTY(QString libraryPath READ libraryPath NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryName READ libraryName NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryError READ libraryError NOTIFY libraryChanged)
    // Whether that is because the library isn't there: retried, or made, on request (#221).
    Q_PROPERTY(bool libraryMissing READ libraryMissing NOTIFY libraryChanged)
    // Bumped whenever the books change, for bindings that read book().
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY busyChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    // The words of a button that acts on the message, when a plugin's notice
    // has one (omabook.ui.notify, #135); empty for none.
    Q_PROPERTY(QString messageAction READ messageAction NOTIFY messageChanged)
    // How long the message is shown before it goes by itself, in milliseconds;
    // 0 for one that waits for the person to put it away (#233).
    Q_PROPERTY(int messageLasts READ messageLasts NOTIFY messageChanged)
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
    // The plugins' microphone and speakers, and who records now, which the
    // window shows (RecordingMark.qml).
    Q_PROPERTY(AudioDevices *audio READ audio CONSTANT)
    // What writes out speech here, and installing voxtype where Omarchy can (#146).
    Q_PROPERTY(SpeechInstall *speech READ speech CONSTANT)
    // Dictating into a note field, and whether it listens now (#148).
    Q_PROPERTY(Dictation *dictation READ dictation CONSTANT)
    // An answer to a question asked by voice, read aloud (#149).
    Q_PROPERTY(SpokenAnswer *spokenAnswer READ spokenAnswer CONSTANT)
    // Writing out the library's recordings (#151).
    Q_PROPERTY(TranscriptDesk *transcripts READ transcripts CONSTANT)
    // What is said while reading, understood as a command (#150).
    Q_PROPERTY(VoiceCommands *voiceCommands READ voiceCommands CONSTANT)
    // The plugin manager: every plugin sorted by what it does, what needs
    // attention, and installing, removing, ordering and trying them.
    Q_PROPERTY(PluginManager *pluginManager READ pluginManager CONSTANT)
    // Building the knowledge graph for books after the fact.
    Q_PROPERTY(GraphJobs *graphJobs READ graphJobs CONSTANT)
    // Publishing a collection for others, and subscribing to one of theirs.
    Q_PROPERTY(SpaceDesk *spaces READ spaces CONSTANT)
    // The library's view in the file manager, kept in step while the window
    // runs, and whether Omabook opens ebooks by default.
    Q_PROPERTY(Folio::Follower *fileManager READ fileManager CONSTANT)
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
    // The library's collections (library.h), as a tree read from the top,
    // each as {"id", "name", "path", "parent", "depth", "children", "rule",
    // "count", "included", "excluded", "format", and of one a subscribed
    // space fills "space", "sharedBy", its owner's name, and "followed",
    // whether from a link}; and
    // the one the shelf shows, 0 for all the books. Each collection keeps
    // its own view, stacks and order, so going from one to another shows
    // each as it was left. Which one is shown is remembered per library.
    Q_PROPERTY(QVariantList collections READ collections NOTIFY collectionsChanged)
    Q_PROPERTY(qint64 collection READ collection WRITE setCollection NOTIFY collectionsChanged)
    Q_PROPERTY(QString collectionName READ collectionName NOTIFY collectionsChanged)

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

    BookListModel *books() noexcept { return &m_model; }
    Library *library() noexcept { return m_library.get(); }
    Folio::Follower *fileManager() noexcept { return &m_fileManager; }
    Settings *settings() noexcept { return m_settings; }
    QString libraryPath() const { return m_library ? m_library->root() : QString(); }
    QString libraryName() const;
    QString libraryError() const noexcept { return m_libraryError; }
    bool libraryMissing() const noexcept { return m_libraryMissing; }
    int revision() const noexcept { return m_revision; }
    QString view() const noexcept { return m_view; }
    void setView(const QString &view);
    QString group() const noexcept { return m_group; }
    void setGroup(const QString &group);
    static QStringList views() { return {"covers", "spines", "carousel", "list", "stacks"}; }
    static QStringList groupings() { return {"series", "author", "tag", "status"}; }
    QVariantList collections() const noexcept { return m_collections; }
    qint64 collection() const noexcept { return m_collection; }
    void setCollection(qint64 id);
    QString collectionName() const;
    // The books of the collection shown, or of the library when it is all of them.
    QList<Book> shownBooks() const;

    // The collection after (1) or before (-1) the one shown, "All books" among them.
    Q_INVOKABLE void stepCollection(int by);
    // Makes a collection (id 0) or changes one: its name and rule. Its
    // number, or 0 with what was wrong said in the window.
    // `parent` is the collection it goes inside, 0 for the top.
    Q_INVOKABLE qint64 saveCollection(qint64 id, const QString &name, const QString &rule, qint64 parent = 0);
    Q_INVOKABLE void removeCollection(qint64 id);
    // How many books a rule finds, and what is wrong with it ("" when nothing).
    Q_INVOKABLE int ruleCount(const QString &rule) const;
    Q_INVOKABLE QString ruleProblem(const QString &rule) const;
    // The search and the filters the shelf has now, written as a rule.
    Q_INVOKABLE QString shelfRule() const;
    // The collections and whether a book is in each: [{"id", "name", "in", "byHand"}].
    Q_INVOKABLE QVariantList bookCollections(qint64 bookId) const;
    // Puts a book in a collection, or takes it out, by hand.
    Q_INVOKABLE void setInCollection(qint64 collectionId, qint64 bookId, bool in);
    bool busy() const noexcept { return m_jobs > 0; }
    QString status() const noexcept { return m_status; }
    QString message() const noexcept { return m_message; }
    QString messageAction() const { return m_message == m_notice.text ? m_notice.action : QString(); }
    [[nodiscard]] int messageLasts() const;
    bool searching() const noexcept { return m_pendingSources > 0; }
    QVariantList candidates() const;
    QString searchProblem() const noexcept { return m_searchProblem; }
    QVariantList lookUpSources() const;
    QVariantList plugins() const;
    Q_INVOKABLE void setPluginEnabled(const QString &id, bool enabled);
    // Asks Calibre again which plugins it has, beside the window.
    Q_INVOKABLE void refreshPlugins();

    bool calibreSwitchedOff() const noexcept { return m_calibreSwitchedOff; }
    QVariantList calibreLibraries() const noexcept { return m_calibreLibraries; }
    QVariantMap calibreOffer() const { return m_calibreOffers.value(0).toMap(); }
    // Looks for Calibre libraries because the user asked: offers every one
    // not imported or refused for good, and says so when there is none.
    Q_INVOKABLE void findCalibreLibraries();
    // The answer to the library on offer: "import", "later" or "never".
    Q_INVOKABLE void answerCalibreOffer(const QString &answer);
    // Imports a Calibre library, whatever was answered before.
    Q_INVOKABLE void importCalibreLibrary(const QString &path);
    Plugins *pluginRegistry() noexcept { return &m_plugins; }
    // The script plugins. Backend tells them what happens to the library;
    // the reader tells them what happens to the open book.
    ScriptPlugins *scripts() noexcept { return m_scripts.get(); }
    AudioDevices *audio() noexcept { return &m_audio; }
    SpeechInstall *speech() noexcept { return &m_speech; }
    Dictation *dictation() noexcept { return &m_dictation; }
    SpokenAnswer *spokenAnswer() noexcept { return &m_spokenAnswer; }
    TranscriptDesk *transcripts() noexcept { return &m_transcripts; }
    VoiceCommands *voiceCommands() noexcept { return &m_voiceCommands; }
    PluginManager *pluginManager() noexcept { return m_pluginManager.get(); }
    GraphJobs *graphJobs() noexcept { return m_graphJobs; }
    SpaceDesk *spaces() noexcept { return &m_spaces; }

    // For what works on the whole library (the librarian): a model, the
    // sources a look-up asks, a book's opening pages, and a cover fetched.
    std::unique_ptr<AiProvider> newAi(QString *why) const { return m_aiFactory(why); }
    // The same, for work beside the window: a copy of it owes nothing to
    // this object, so it can be called after this is gone.
    const AiFactory &aiFactory() const noexcept { return m_aiFactory; }
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
    // With "writeToFile" (or else the setting) the details go into its EPUB too.
    Q_INVOKABLE void saveDetails(qint64 id, const QVariantMap &details);
    // A book's EPUB and the details in it (#74): {epub: whether it has one,
    // original: whether the file as it came in is kept, to put back}.
    Q_INVOKABLE QVariantMap fileDetails(qint64 id) const;
    // Its details written into its EPUB, or that file put back as it came in,
    // beside the window; the shelf is read again once it is done.
    Q_INVOKABLE void writeDetails(qint64 id);
    Q_INVOKABLE void restoreFile(qint64 id);
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
    // What a sync did, a line each, as the window says it: with a hub too (#189).
    [[nodiscard]] static QStringList syncLines(const SyncSummary &summary);
    // What the window says when books have been added (backendimport.cpp).
    [[nodiscard]] static QStringList importLines(const ImportSummary &summary, bool calibreLeftOut);
    Q_INVOKABLE void chooseLibrary();
    Q_INVOKABLE void useLibrary(const QString &path);
    // For a library that isn't there: look again, or start a new one in its folder.
    Q_INVOKABLE void retryLibrary();
    Q_INVOKABLE void createLibrary();
    // Takes a library off the list; with `trash` its folder goes to the
    // trash too. What went wrong, if anything, is said in the window.
    Q_INVOKABLE void forgetLibrary(const QString &path, bool trash = false);
    Q_INVOKABLE void dismissMessage();
    Q_INVOKABLE void say(const QString &message);
    // As say(), for how work beside the window ended: when something went
    // wrong the message needs the person, and stays until it is put away.
    void sayOutcome(const QString &message, bool wentWrong);
    // Presses the message's button: its plugin is told, and the message goes.
    Q_INVOKABLE void actOnMessage();
    // Asks for a reading site's export file to read, or where to write
    // one; the answer is readingFilePicked.
    void pickReadingFile(bool save, const QString &suggested = {});

public slots:
    void reload();
    void reloadIfChanged();

signals:
    void libraryChanged();
    void revisionChanged();
    // A book's own reading format was changed, here or elsewhere.
    void bookFormatsChanged();
    // The year's reading goal was set in the window (#68), for every view of it.
    void readingGoalChanged();
    void busyChanged();
    void messageChanged();
    void candidatesChanged();
    void pluginsChanged();
    void calibreChanged();
    void shelfChanged();
    void collectionsChanged();
    // A book was added by the user's own doing, so the shelf can select it.
    void bookAdded(qint64 id);
    void readingFilePicked(bool save, const QString &file);

private:
    // Opens the library at `path`; `create` makes one where one is expected and isn't (backendlibrary.cpp).
    void openLibrary(const QString &path, bool create = false);
    void switchTo(const QString &path, bool create);
    void showLibrary(std::unique_ptr<Library> library, const QString &error, bool missing);
    void lookForLibrary();
    [[nodiscard]] static QString notThere(const QString &path);
    // The script plugins, once there is a library (backendplugins.cpp).
    void startScripts(const QString &pluginFolder);
    // Reads how the library's shelf was left from the settings, and writes it there.
    void takeShelf();
    void keepShelf();
    // Shows the shelf as the collection in view keeps it (or the library,
    // for all the books): its view, its stacks and its order.
    void showCollectionShelf();
    // Writes a change to the view or the order into the collection in view.
    void keepCollectionShelf();
    // Reads the collections again, with how many books each has.
    void readCollections(const QList<Book> &books);
    // Books added by the user while a collection is shown go into it.
    void putInShownCollection(const QList<qint64> &ids);
    void watchLibrary();
    // Reads what the knowledge graph knows of the books for the search,
    // beside the window, when the search wants it and what was read last
    // is out of date.
    void buildKnowledge();
    void readKnowledge();
    void picked(FilePicker::Purpose purpose, const QList<QUrl> &urls);
    // What was picked for the book asked about (m_pickingFor): its cover, and where a copy goes.
    void useCover(const QString &file);
    void saveCopy(const QString &target);
    void importFinished(const ImportSummary &summary, bool calibreLeftOut);
    void writeDetailsWhenAsked(qint64 id, bool asked);
    void changeFile(qint64 id, bool restore);
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
    bool m_libraryMissing = false;
    QTimer m_presence;
    BookListModel m_model;
    QString m_view = "covers";
    QString m_group = "series";
    // How the whole library is shown, whatever collection is in view.
    QString m_libraryView = "covers";
    QString m_libraryGroup = "series";
    qint64 m_collection = 0;
    QVariantList m_collections;
    // Set while the shelf is read from the settings or written to them, so
    // one doesn't answer the other.
    bool m_shelfMoving = false;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadSoon;
    Folio::Follower m_fileManager{m_settings};
    int m_revision = 0;
    // What the library's change counters said when it was last read.
    Library::Changes m_changes;
    // The knowledge being read for the search, and whether it is wanted
    // again once that is done.
    bool m_knowing = false;
    bool m_knowAgain = false;
    // Read again only so often while the graph keeps growing (as it does
    // while a book is read), in proportion to how long a reading takes.
    QTimer m_knowSoon;
    qint64 m_knowingTook = 0;
    int m_jobs = 0;
    QString m_status;
    QString m_message;
    bool m_messageStays = false;
    // The last notice a plugin gave with a button, while it is the message.
    struct Notice {
        QString text, action, plugin;
        int token = 0;
    };
    Notice m_notice;
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
    // Before the plugins, which use it until they go.
    AudioDevices m_audio;
    SpeechInstall m_speech{m_settings};
    Dictation m_dictation{m_settings, &m_audio};
    SpokenAnswer m_spokenAnswer{m_settings, &m_audio};
    // Before the plugins too, which write their recordings out through it.
    TranscriptDesk m_transcripts{m_settings, [this] { return m_library.get(); }};
    // The model it falls back on is the one every other answer comes from.
    VoiceCommands m_voiceCommands{m_aiFactory};
    // Files the person picks for a plugin, and the clipboard (#131).
    PluginDesk m_pluginDesk{m_picker};
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
    SpaceDesk m_spaces{*this, m_picker};
};
