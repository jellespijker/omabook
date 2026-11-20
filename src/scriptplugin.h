#pragma once

#include <QHash>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <optional>

#include "library.h"
#include <QThread>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include "formatprovider.h"
#include "scriptwatchdog.h"

class AiProvider;
class QNetworkAccessManager;
class Settings;

// What a script plugin is given to work with. Each is optional: a plugin
// checked from the command line has no window, and may have no library.
struct ScriptServices {
    Settings *settings = nullptr;
    // The library that is open, or null.
    std::function<Library *()> library;
    // A model to ask, or null with the reason; as Backend::AiFactory.
    std::function<std::unique_ptr<AiProvider>(QString *why)> ai;
    QNetworkAccessManager *network = nullptr;
    // The book open in the reader: {id, section, fraction, percent}, or empty.
    std::function<QJsonObject()> reading;
};

// plugin.json of a script plugin:
//   {"name": "Gutenberg", "version": "1.0", "author": "...", "description": "...",
//    "script": "main.js", "permissions": ["http", "files", "run"]}
struct ScriptManifest {
    QString id;          // the folder's name, which is what the command line calls it
    QString name, version, author, description;
    QString script;      // empty for a folder plugin that isn't a script
    QString folder;
    QStringList permissions;
    QString problem;     // why it can't be used as it is, if it can't
    bool needsNewer = false; // the problem is that it asks for a newer Omabook than this

    static ScriptManifest read(const QString &folder);
    bool isScript() const noexcept { return !script.isEmpty(); }
    QString dataFolder() const { return folder + "/data"; }
};

class ScriptAi;
class ScriptHost;
class ScriptPlugin;
class ScriptSharing;

// The way into a plugin from another thread. A format a script adds is asked
// for books from wherever a book is opened (the reader opens them beside the
// window), while the script's engine lives on one thread. So a provider
// holds one of these rather than the plugin: it runs the work on the
// plugin's thread and waits for it, at once when the caller is that thread.
// It outlives the plugin, and says so once the plugin is gone; and it never
// waits for ever, so a thread that is itself waited for by the plugin's is
// let go with an error rather than left hanging.
class ScriptGate : public std::enable_shared_from_this<ScriptGate> {
public:
    explicit ScriptGate(ScriptPlugin *plugin);

    // The plugin is going away, or the app is: whoever waits is let go.
    void close();
    // Runs `work` on the plugin's thread and waits for it, `patience` at
    // most. False, with why, when it was never run or didn't finish in
    // time; `work` may then still be running, so everything it touches
    // must be its own (captured by value, or shared).
    bool run(const std::function<void(ScriptPlugin &)> &work, std::chrono::milliseconds patience, QString *error);

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    ScriptPlugin *m_plugin; // null once closed; read under the lock off its thread
    QThread *const m_thread;
};

// One plugin's script in an engine of its own. Everything that calls into
// the script goes through here: it is timed, an error it throws is reported
// rather than raised, and what it answers (a value or a promise of one)
// arrives in a callback. Nothing here knows of the window.
class ScriptPlugin : public QObject {
    Q_OBJECT

public:
    struct Source {
        QString name;
        QStringList fields;
        QJSValue search;
    };
    struct Catalog {
        QString format, description;
        QJSValue make;
    };
    struct Importer {
        QString name;
        QJSValue added;
    };
    struct Command {
        QString id;     // "<place>.<plugin>:<id>", as keys.json names it
        QString title, place, key;
        QJSValue run;
        QString when;        // "always", or "generated": only for a book this plugin made
        bool enabled = true; // what the script last said with commands.setEnabled
    };
    struct Panel {
        QString place, title, source; // the source as a path
        int width = 280;
        QString when;        // as a command's
        bool visible = true; // what the script last said with ui.setVisible
    };
    struct Format {
        QString name;
        QStringList extensions;
        QJSValue claims, open; // claims is not callable when the script gave none
    };
    // An entry on the shelf that is not a book: opening it calls the
    // script (omabook.ui.launcher). Nothing of it is in the library.
    struct Launcher {
        QString id, title, author, description;
        QString cover;  // a picture in the plugin's folder, as a path; empty for one drawn from the theme
        QJSValue open, removed; // removed is not callable when the script gave none
    };
    // What a call answered, or why it failed (with file and line when known).
    using Done = std::function<void(const QJsonValue &result, const QString &problem)>;

    ScriptPlugin(const ScriptManifest &manifest, const ScriptServices &services, QObject *parent = nullptr);
    ~ScriptPlugin() override;

    // Runs the script's top level. False, with `problem`, when it throws.
    bool load(QString *problem);
    const ScriptManifest &manifest() const noexcept { return m_manifest; }
    // How long a call may run before it is stopped; five seconds unless said.
    void setTimeLimit(std::chrono::milliseconds limit) { m_limit = limit; }

    const QList<Source> &sources() const noexcept { return m_sources; }
    const QList<Catalog> &catalogs() const noexcept { return m_catalogs; }
    const QList<Importer> &importers() const noexcept { return m_importers; }
    const QList<Command> &commands() const noexcept { return m_commands; }
    const QList<Panel> &panels() const noexcept { return m_panels; }
    const QList<Format> &formats() const noexcept { return m_formats; }
    const QList<Launcher> &launchers() const noexcept { return m_launchers; }
    // Opening a launcher, and the reader taking it off the shelf: the
    // script is told, and the launcher goes until the script puts it back.
    void launch(const QString &id, const Done &done = {});
    void dropLauncher(const QString &id);
    // A format of the script's as the Formats layer takes it (see
    // formatprovider.h): safe to call from any thread, and after the plugin
    // is gone, when it answers that the plugin was switched off.
    FormatProvider provider(const Format &format) const;
    // The same two questions, asked on the plugin's own thread. The book is
    // what the script's `open` answered, as JSON; described() makes of it
    // what the Formats layer wants, on any thread.
    bool claimsBook(const QString &format, const BookSource &source);
    QJsonObject openBook(const QString &format, const BookSource &source, QString *problem);
    static std::optional<BookDescription> described(const QJsonObject &book, QString *problem);
    // Stops answering for the script's formats from other threads.
    void closeGate() { m_gate->close(); }
    // What of the plugin's goes into spaces (omabook.sharing).
    ScriptSharing &sharing() { return *m_sharing; }
    // The events the script listens to.
    QStringList events() const;
    // What the script set for its panels.
    QJsonObject data() const { return m_data; }

    void emitEvent(const QString &event, const QJsonObject &payload);
    void search(const QString &source, const QJsonObject &book, const Done &done);
    void makeCatalog(const QString &format, const QJsonArray &books, const Done &done);
    void imported(const QString &importer, const QJsonObject &book, const Done &done);
    void runCommand(const QString &id, const QJsonObject &context, const Done &done = {});
    // A function the script defines at its top level.
    void callFunction(const QString &name, const QJsonArray &arguments, const Done &done);
    // The same, for a caller that can't wait: what it answers at once.
    QJsonValue callNow(const QString &name, const QJsonArray &arguments, QString *problem);

    // What the reader typed for an omabook.ui.ask, or null when they
    // dismissed it.
    void answerAsk(int token, const QJsonValue &text);
    // What was filled in on a form (ScriptForm::values), or null for none.
    void answerForm(int token, const QJsonValue &values);

    // Whether nothing the script started is still under way: no promise
    // unsettled, no timer, request or program running.
    bool idle() const { return m_busy == 0; }
    // Whether every call into the script has been answered (a promise it
    // returned included) and nothing it asked to be waited for is pending.
    // Timers and requests it merely started don't count: that is when a
    // command that ran one thing of the plugin's is done.
    bool answered() const { return m_waiting.isEmpty() && m_holds == 0; }
    // The lines the script logged since the last time this was asked.
    QStringList takeLog();

    // The `omabook` object as the script sees it, for the test that holds
    // it against the table in scriptapi.cpp: every function and value by
    // its path ("library.get").
    QStringList surface();

signals:
    void said(const QString &text);
    // Something the script did went wrong after it loaded.
    void failed(const QString &problem);
    // The script added or took away a source, command, panel or handler.
    void registered();
    void dataChanged();
    // The script changed the library.
    void touchedLibrary();
    // Opens a book, at a place in it when `section` is not negative.
    void openBookRequested(qint64 id, int section, double fraction);
    // The window should show the knowledge graph: of a book, else of a scope.
    void graphRequested(const QString &scope, qint64 book);
    // The reader should open this book's file anew and stay where it is:
    // the book grew. 0 is whichever book is open.
    void reloadBookRequested(qint64 id);
    // The script wants words from the reader (omabook.ui.ask); answerAsk
    // gives them, or says there are none.
    void askRequested(int token, const QString &title, const QString &placeholder, const QString &text);
    // The same for a form (omabook.ui.form), checked by ScriptForm::checked;
    // answerForm answers it.
    void formRequested(int token, const QJsonObject &form);
    // Something the script started has finished; see idle().
    void settled();

private:
    friend class ScriptAi;
    friend class ScriptHost;
    friend class ScriptSharing;

    QJSValue fromJson(const QJsonValue &value);
    QString describe(const QJSValue &error) const;
    // Calls into the script, timed. An error comes back as `problem`.
    QJSValue call(const QJSValue &function, const QJSValueList &arguments, QString *problem);
    // Calls, then waits for what was answered if it is a promise.
    void invoke(const QJSValue &function, const QJsonValue &argument, const Done &done);
    void enter();
    void leave();
    void interrupted();
    void busy(int change);
    void report(const QString &problem);
    // A path of the script's as a real one inside the plugin's folder (its
    // data folder, to write), or empty when it leads anywhere else.
    QString inside(const QString &path, bool writing) const;
    QJsonObject kept() const;

    ScriptManifest m_manifest;
    ScriptServices m_services;
    std::chrono::milliseconds m_limit{5000};
    QJSEngine m_engine;
    std::unique_ptr<ScriptHost> m_host;
    std::unique_ptr<ScriptSharing> m_sharing;
    std::unique_ptr<ScriptAi> m_ai;
    QJSValue m_settle, m_parse;
    int m_depth = 0;
    // The library write the code running now makes, as one transaction.
    std::optional<Library::Batch> m_batch;
    int m_busy = 0;
    int m_holds = 0;
    int m_nextToken = 1;
    QHash<int, Done> m_waiting;
    QHash<int, QJSValue> m_asks;
    QList<Source> m_sources;
    QList<Catalog> m_catalogs;
    QList<Importer> m_importers;
    QList<Command> m_commands;
    QList<Panel> m_panels;
    QList<Format> m_formats;
    QList<Launcher> m_launchers;
    // The books being opened or looked at right now, by the number the
    // script's handle on each carries; a handle kept is good for nothing.
    QHash<int, const BookSource *> m_books;
    QJSValue m_sourceHandle, m_bookJson;
    std::shared_ptr<ScriptGate> m_gate;
    QHash<QString, QList<QJSValue>> m_handlers;
    QJsonObject m_data;
    QStringList m_log;
    // Last, so it stops watching before the engine it interrupts is gone.
    ScriptWatchdog m_watchdog;
};
