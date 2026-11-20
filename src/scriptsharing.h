#pragma once

#include <QJSValue>
#include <QList>
#include <QObject>
#include <QString>
#include <memory>

#include "scriptchangelog.h"
#include "spaceworlds.h"

class Library;
class ScriptPlugin;
class ScriptPlugins;

// What of a script plugin's goes into spaces, and comes back from them
// (#65): the C++ side of `omabook.sharing`, `omabook.graph.layers` and
// `omabook.spaces`, beside ScriptHost (scriptplugin.cpp), which asks it
// before a plugin writes to a scope.
//
// A plugin registers what a collection takes of its own (worlds); Omabook
// asks it when the collection is published or shared, and gives the space
// only what SpaceWorlds allows. The scopes it writes to first are noted as
// its own (SpaceWorlds::madeBy), the one way a scope becomes a plugin's. The
// worlds others shared are layers (WorldLayers), which it reads and copies
// and never writes to.
class ScriptSharing : public QObject {
    Q_OBJECT

public:
    explicit ScriptSharing(ScriptPlugin *plugin) : m_plugin(plugin) {}

    // What the plugin offers of the books `asked` names; nothing when it
    // registered nothing, or what it answered is no list of them (said in
    // its log).
    [[nodiscard]] QList<SpaceWorlds::Offer> offers(const SpaceWorlds::Asked &asked);

    // Before the plugin writes to `scope`: false, with an error thrown into
    // the script, for a layer's; else true, and a scope that isn't there yet
    // is the plugin's.
    bool writing(const Library &library, const QString &scope);
    // graph.copyScope, from a layer as well.
    bool copyScope(const Library &library, const QString &from, const QString &to);

    Q_INVOKABLE void registerWorlds(const QJSValue &worlds);
    Q_INVOKABLE QString layers();
    Q_INVOKABLE void addToSpace(const QString &space, double book);
    // The plugin's own data in the change log, which travels between one's
    // computers (omabook.log.set and the rest, #76).
    Q_INVOKABLE QObject *changeLog();

private:
    [[nodiscard]] Library *library();
    void fail(const QString &why);

    ScriptPlugin *m_plugin;
    QJSValue m_worlds;
    std::unique_ptr<ScriptChangeLog> m_changeLog;
};

// What every running plugin of `plugins` offers, as SpaceWorlds asks it.
// `plugins` outlives what this answers.
[[nodiscard]] SpaceWorlds::Ask worldsOf(ScriptPlugins &plugins);
