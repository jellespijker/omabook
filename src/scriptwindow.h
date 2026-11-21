#pragma once

#include <QHash>
#include <QJSValue>
#include <QObject>
#include <QString>

class ScriptPlugin;

// What a script plugin puts in the window (#77): the C++ side of
// `omabook.ui.panel`, `ui.view`, `ui.launcher` and what goes with them, beside
// ScriptHost (scriptplugin.cpp), which hands it to scriptui.js. What it is
// given is kept in the plugin (ScriptPlugin::panels, launchers); a file it
// names is one of the plugin's own folder, or it throws into the script.
//
// A view is a panel whose place is "view": a QML file shown in place of the
// shelf, or beside the page while a book is open. Asking for one to be shown
// or closed is a signal of the plugin's, which the window follows.
class ScriptWindow : public QObject {
    Q_OBJECT

public:
    explicit ScriptWindow(ScriptPlugin *plugin) : m_plugin(plugin) {}

    // What scriptui.js makes of this: the function scriptprelude.js builds
    // `omabook.ui` with (as ScriptAi::build does for omabook.ai).
    [[nodiscard]] QJSValue build(QJSEngine &engine);

    Q_INVOKABLE void registerPanel(const QString &place, const QString &title, int width, const QString &source, const QString &when);
    Q_INVOKABLE void setPanelVisible(const QString &source, bool visible);
    // A view, by an id of the plugin's: the same id again replaces it.
    Q_INVOKABLE void registerView(const QString &id, const QString &title, int width, const QString &source);
    // `args` is JSON; `toggle` puts away a view that is shown already (its key does).
    Q_INVOKABLE void showView(const QString &id, const QString &args, bool toggle);
    Q_INVOKABLE void closeView(const QString &id);
    // `launcher` is JSON: {id, title, author, description, cover}.
    Q_INVOKABLE void registerLauncher(const QString &launcher, const QJSValue &open, const QJSValue &removed);
    Q_INVOKABLE void removeLauncher(const QString &id);
    // Says something briefly in the window (#135), with a button `action`
    // that calls `run` when it is given; with no window, it is said as
    // omabook.say says it.
    Q_INVOKABLE void notify(const QString &text, const QString &action, const QJSValue &run);
    // The person pressed the button of the notice with this number; once.
    void act(int token);

private:
    // The file a panel or view names, as a path in the plugin's folder; empty, with an error thrown, when there is none.
    [[nodiscard]] QString source(const QString &file, const QString &what);
    [[nodiscard]] bool knowsView(const QString &id);
    void fail(const QString &why);

    ScriptPlugin *m_plugin;
    // What a notice's button calls, by its number; the latest few.
    QHash<int, QJSValue> m_actions;
};
