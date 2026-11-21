#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <map>
#include <memory>

#include "library.h"
#include "metadatasource.h"
#include "pluginnews.h"
#include "scriptplugin.h"

// The script plugins in ~/.config/omabook/plugins: each folder there whose
// plugin.json names a script, loaded into an engine of its own and followed
// while the app runs, the way Customization follows the interface's files.
// A plugin that doesn't load, or throws later, says why in <name>.error
// beside its folder and the last version that loaded keeps running.
//
// This is where the rest of Omabook meets the scripts: it is told what
// happens (emitEvent, libraryChanged) and asked for what the scripts added
// (sources of details, catalog formats, commands, panels). It knows nothing
// of the window; Backend and Reader feed it, and the command line uses it
// with no window at all.
class ScriptPlugins : public QObject {
    Q_OBJECT
    // What the window needs of the plugins: their commands as
    // [{id, title, place, key, plugin}], their panels as mods (see
    // Customization::mods), and what each script set with omabook.ui.set,
    // by plugin.
    Q_PROPERTY(QVariantList commands READ commands NOTIFY changed)
    Q_PROPERTY(QVariantList panels READ panels NOTIFY changed)
    Q_PROPERTY(QVariantMap data READ data NOTIFY dataChanged)
    // What a script is asking the reader for words about (omabook.ui.ask):
    // {id, plugin, title, placeholder, text}, or empty when nothing is asked.
    Q_PROPERTY(QVariantMap question READ question NOTIFY questionChanged)
    // The book selected on the shelf, which the window says. With the book
    // open in the reader (which the events say) it is "the book in front of
    // the reader": what a command or panel that is only for a plugin's own
    // books goes by.
    Q_PROPERTY(qint64 selectedBook READ selectedBook WRITE setSelectedBook NOTIFY selectedBookChanged)

public:
    struct Info {
        ScriptManifest manifest;
        bool enabled = true;
        bool loaded = false;
        QString problem; // why it isn't running, or what last went wrong in it
        QStringList sources, catalogs, importers, commands, events, panels;
        // The book formats it reads, each as "Name (.ext, .ext)".
        QStringList formats;
        QJsonObject toJson() const;
    };

    // `folder` is the plugin folder (tests pass a temp one). With `follow`
    // off the folder is read once and not watched, as a command wants it.
    explicit ScriptPlugins(const ScriptServices &services, const QString &folder = {}, QObject *parent = nullptr,
                           bool follow = true);
    ~ScriptPlugins() override;

    QString folder() const { return m_folder; }
    static QString defaultFolder();
    // Every script plugin in the folder, running or not.
    QList<Info> list() const;
    // A running plugin by its folder's name, or null.
    ScriptPlugin *plugin(const QString &id) const;
    // Reads the folder now, without waiting to be told it changed.
    void reload();
    // Loads one plugin again though nothing of it changed: what "try again"
    // does for one that went wrong for a reason outside its files.
    void retry(const QString &id);
    // Has the book formats the scripts register join the ones Omabook
    // reads (Formats::addProvider), and leave when their plugin is switched
    // off, changed or removed. For the one set of plugins a program runs
    // with: the window's, or a command's (ScriptFormats below); a set made
    // for the length of one look-up keeps its formats to itself.
    void provideFormats();
    // How long any call into a script may run; for tests.
    void setTimeLimit(std::chrono::milliseconds limit);

    // Tells every script that listens. See ScriptApi::events().
    void emitEvent(const QString &event, const QJsonObject &payload = {});
    // The books as they are now: what was added, removed or changed since
    // the last time is told to the scripts, and their importers have their
    // say in the new ones. A book is announced once, whenever it came in.
    void libraryChanged(const QList<Book> &books, const QString &libraryPath);
    // Looks at the library's change log for data of a plugin's that came from
    // elsewhere (a sync, a pairing, a space, operations kept until known), and
    // a moment after the last of it tells each running plugin of its own
    // alone: `log.changed`, and logChanged for its view (scriptpluginlog.cpp).
    void lookAtTheLog();

    // A book as the scripts are told of it: what `omabook list --json` prints,
    // with its global id (ScriptBook::of).
    QJsonObject told(const Book &book) const;
    // The words the reader selected in the open book, as a passage of it
    // ({book, global_id, passage}), or none (empty), which the window says a
    // moment after the selection changes: `omabook.reader.selection()`, and
    // `reader.selected` to every plugin when it is another (#137).
    void setSelection(const QJsonObject &selection);
    // Shows a book that is a running plugin's own kind (a format it reads
    // with a view, omabook.format.register or plugin.json) in that view, with
    // {book} as its args, in place of the reader (#125). False for any other
    // book, and for one with a file Omabook reads itself.
    bool openInView(const Book &book);

    // A source for each one the scripts registered, owned by `parent`.
    QList<MetadataSource *> metadataSources(QObject *parent) const;
    // The catalog formats the scripts add, with what each says of itself.
    QStringList catalogFormats() const;
    QString describeCatalog(const QString &format) const;
    // The catalog of these books; waits for the script. Null, with
    // `problem`, when no script writes that format or it failed.
    QByteArray makeCatalog(const QString &format, const QList<Book> &books, QString *problem);

    // Every command, each with whether it applies now ("applies": it is
    // enabled, and not one for the plugin's own books while another book
    // is in front of the reader); and the panels that apply, a plugin's
    // views among them as of the place "view", with their "plugin" and "view".
    QVariantList commands() const;
    QVariantList panels() const;
    qint64 selectedBook() const noexcept { return m_selectedBook; }
    void setSelectedBook(qint64 id);
    QVariantMap data() const;
    // Runs a command for the book that is selected or open. The command is
    // named whole ("library.finished:mark") or by plugin and id
    // ("finished:mark"); the id alone ("mark") only when one plugin has it.
    Q_INVOKABLE void run(const QString &command, qint64 book = 0);
    // Calls a top-level function of a plugin's script and answers later, a
    // promise it returns included: the number given back comes again in
    // `answered`, with what the function answered or why it failed.
    Q_INVOKABLE int request(const QString &plugin, const QString &function, const QVariantList &arguments = {});
    // Calls a top-level function of a plugin's script and answers what it
    // returned at once; for panels.
    Q_INVOKABLE QVariant call(const QString &plugin, const QString &function, const QVariantList &arguments = {});

    QVariantMap question() const;
    // What a form (omabook.ui.form) is answered: the values filled in, of
    // the shape the form asked for. False, and the form stays, while a field
    // that must be filled in is empty.
    Q_INVOKABLE bool fill(int id, const QVariantMap &values);
    // For the window drawing a form: the values as they would be answered,
    // whether a field is shown with them, and which field still needs words.
    Q_INVOKABLE QVariantMap formValues(const QVariantMap &form, const QVariantMap &values) const;
    Q_INVOKABLE bool fieldShown(const QVariantMap &field, const QVariantMap &values) const;
    Q_INVOKABLE QString formMissing(const QVariantMap &form, const QVariantMap &values) const;

    // The entries plugins put on the shelf (omabook.ui.launcher), each as a
    // book is shown: {bookId, plugin, pluginName, launcher, title, authors,
    // description, cover}. A launcher's bookId is below zero, so no book
    // has it, and it is the same for as long as Omabook runs.
    QVariantList launchers();
    Q_INVOKABLE QVariantMap launcher(qint64 number);
    // Opening one calls its plugin, not the reader; false when there is no
    // such launcher (its plugin was switched off meanwhile).
    Q_INVOKABLE bool launch(qint64 number);
    // The reader took it off the shelf; its plugin is told.
    Q_INVOKABLE bool removeLauncher(qint64 number);
    // The window says it is there to put questions to the reader, or no
    // longer is. With nobody attending, a script that asks is answered null.
    Q_INVOKABLE void attend(bool attending);
    // The reader's words for the question with this id, or their leaving it.
    Q_INVOKABLE void answer(int id, const QString &text);
    Q_INVOKABLE void dismiss(int id);
    // The person pressed the button of a plugin's notice.
    Q_INVOKABLE void act(const QString &plugin, int token) const;

    // Lets everything the scripts started finish (promises, requests,
    // programs), for a caller with no event loop of its own. False when the
    // time ran out first.
    bool wait(int milliseconds = 30000);
    // The same for anything: turns the event loop until `idle` has held for
    // a few turns, or the time runs out.
    static bool waitFor(const std::function<bool()> &idle, int milliseconds);

signals:
    // A plugin was loaded, reloaded, switched or removed, or registered something.
    void changed();
    void dataChanged();
    void said(const QString &text);
    void touchedLibrary();
    void openBookRequested(qint64 id, int section, double fraction);
    // The window should show the knowledge graph of a book, else of a scope.
    void graphRequested(const QString &scope, qint64 book);
    // The window should show a plugin's view (omabook.ui.showView, and its
    // commands, which `toggle`), or put it away; by the plugin's folder name
    // and the view's id, as `panels` has them.
    void viewRequested(const QString &plugin, const QString &view, const QVariantMap &args, bool toggle);
    void viewClosed(const QString &plugin, const QString &view);
    // A plugin said something briefly (omabook.ui.notify), with a button
    // that acts on it when `action` isn't empty: act() presses it (#135).
    void notified(const QString &plugin, const QString &text, const QString &action, int token);
    // What a `request` came to.
    void answered(int id, const QVariant &result, const QString &error);
    void selectedBookChanged();
    // The reader should read this book's file anew, keeping its place (0:
    // whichever book is open).
    void reloadBookRequested(qint64 id);
    void questionChanged();
    // A plugin's data came from elsewhere, `{entity: [id, ...]}`: for its
    // view, which is handed it (PluginView.qml).
    void logChanged(const QString &plugin, const QVariantMap &entities);

private:
    void watch();
    void adopt(std::unique_ptr<ScriptPlugin> plugin);
    void report(const QString &id, const QString &problem, bool stillRunning);
    QByteArray stamp(const QString &folder, bool enabled) const;
    // Follows where the reader is from what it tells, and answers what the
    // scripts are told: the same, each book in it with its global id (scriptpluginbooks.cpp).
    QJsonObject follow(const QString &event, const QJsonObject &payload);
    void syncFormats();
    // Marks the books (the library's, for none) that are a running plugin's
    // own as its (PluginClaims); only the window's set does. Whether any was.
    bool claimBooks(const QList<Book> &books = {});
    // The books after claimBooks: these, or the library's anew when it marked any.
    QList<Book> claimed(const QList<Book> &books);
    void listenTo(ScriptPlugin *plugin);
    // What a plugin asks of the window: the graph, its views (scriptpluginviews.cpp).
    void listenToWindow(ScriptPlugin *plugin);
    void lookAtTheBook();
    bool applies(const QString &plugin, const QString &when, bool on) const;
    void settle(int id, const QJsonValue &text);

    struct Question {
        int id = 0;
        QPointer<ScriptPlugin> plugin;
        int token = 0;
        QString title, placeholder, text;
        QJsonObject form; // a form's, checked; empty for a question
    };
    QList<Question> m_questions;
    int m_nextQuestion = 1;
    int m_attending = 0;

    ScriptServices m_services;
    QString m_folder;
    bool m_follow;
    QNetworkAccessManager m_network;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_announce;
    std::chrono::milliseconds m_limit{5000};
    std::map<QString, std::unique_ptr<ScriptPlugin>> m_plugins;
    QHash<QString, QByteArray> m_stamps;
    QHash<QString, QString> m_problems;
    bool m_providesFormats = false;
    QStringList m_formatIds; // the providers this set has in Formats
    QJsonObject m_reading;
    QJsonObject m_selection;
    qint64 m_selectedBook = 0;
    QString m_maker; // the plugin that made the book in front of the reader, if one did
    int m_nextRequest = 1;
    QHash<QString, qint64> m_launcherNumbers; // by "<plugin>:<launcher>"
    qint64 launcherNumber(const QString &plugin, const QString &id);
    // The library as the scripts last heard of it.
    QString m_libraryPath;
    QHash<qint64, QJsonObject> m_books;
    void tellLog(const QString &plugin, const QJsonObject &entities);
    PluginNews::Feed m_logFeed{[this](const QString &plugin, const QJsonObject &entities) { tellLog(plugin, entities); }};
};

// The script plugins for a command, which has no window: read once and not
// followed, working on the library in a folder through a connection of
// their own.
class CommandScripts {
public:
    CommandScripts(Settings *settings, const QString &libraryRoot, const QString &folder = {});

    ScriptPlugins &plugins() { return *m_plugins; }

private:
    Library m_library;
    bool m_opened;
    std::unique_ptr<ScriptPlugins> m_plugins;
};

// Script formats where there is no window. A command opens books through
// Formats like the window does, but loads no plugins of its own accord; with
// this, the script plugins are loaded the first time a format Omabook doesn't
// read itself is asked about, and looked at again when their files change,
// so every command, the MCP server and the file manager's helper read the
// formats plugins add without any of them asking.
namespace ScriptFormats {
void loadOnDemand();
// Forgets the plugins loaded that way, and stops loading them.
void unload();
} // namespace ScriptFormats
