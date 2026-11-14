#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "scripttools.h"

class CalibrePlugin;
class ScriptPlugins;
struct PluginInfo;

// Managing plugins, as functions that answer in JSON: what the plugin
// manager in the window shows and does, what `omabook plugin install` and
// `omabook plugin remove` print, and the MCP tools plugin_install and
// plugin_remove, are all these. None needs the window.
//
// As with ScriptTools, every answer is an object, and one with an "error"
// (English) is a call that failed and changed nothing.
namespace PluginTools {

using Context = ScriptTools::Context;

// --- Looking at them ---

// What a plugin does, which is how the manager sorts them: "metadata" (a
// source of book details), "reading" (a reading site), "script", "program"
// (a program plugin that can't be used as a source), "calibre" (the Calibre
// plugin and what it found in Calibre), "format" (reading, converting and
// adding books), "catalog" or "ai". groups() is the order they are shown in.
QStringList groups();
QString group(const PluginInfo &plugin);
// How a plugin is: "on", "off", "needs" (it is on and can't work until
// something outside it is put right) or "broken" (it is at fault itself).
QString state(const PluginInfo &plugin);
// The program a program plugin runs, when it can't be found; else empty.
QString missingProgram(const PluginInfo &plugin);

// Every plugin, sorted by group, each as everything there is to say of it:
// the registry's own fields, and
//   group, state     as above
//   note             why it is in that state, in English, when it isn't plainly on or off
//   removable        whether it is the user's own, in the plugin folder
//   beneath          the id of the plugin it sits under (what Calibre found sits under "calibre")
//   position, of     sources of details: where it stands in the order they are asked in
//   tryable          whether a look-up can be tried against it
//   running          script plugins: whether it is loaded
//   adds             script plugins: [{what: "source"|"catalog"|"format"|"importer"|"command"|"panel"|"hook",
//                    name, detail}], what it registered
//   error            script plugins: {file, line, message, kept}, the last thing that went
//                    wrong in it, from <name>.error; `kept` when the last version that worked still runs
// `scripts` are the running script plugins, when there are any to ask.
QJsonArray overview(const QList<PluginInfo> &plugins, const ScriptPlugins *scripts = nullptr);

// What needs attention, each naming the plugin to look at:
//   {id, name, issue: "broken"}       a script or program plugin that is at fault
//   {id, name, issue: "missing", program}   a program plugin whose program isn't there
//   {id, name, issue: "calibre-off"}  Calibre switched off though its libraries are here
QJsonArray health(const QJsonArray &overview, const CalibrePlugin &calibre);

// The last error of a script plugin, from <name>.error beside its folder:
// {file, line, message, kept}, or an empty object when there is none.
QJsonObject lastError(const QString &pluginFolder, const QString &name);
// Forgets it. The plugin is left as it is.
bool dismissError(const QString &pluginFolder, const QString &name);

// --- Installing and removing ---

// A folder's name for a plugin from any words: "Reading Log 2" is
// "reading-log-2". Empty when nothing usable is left.
QString folderName(const QString &text);

// How much a plugin may be, so that an archive can't fill the disk: this
// many files, none larger than `fileBytes`, `totalBytes` together.
struct Limits {
    int files = 2000;
    qint64 fileBytes = 16LL * 1024 * 1024;
    qint64 totalBytes = 64LL * 1024 * 1024;
};

struct InstallOptions {
    QString name;         // the folder's name; taken from the package when empty
    bool enable = false;  // a new plugin stays off until its permissions were seen
    bool replace = false; // put it in the place of one of the same name, which goes to the trash
    Limits limits;
};

// Reads a plugin that is not installed yet, from a folder or a .zip, and
// says what it is without putting it anywhere: {name, title, version,
// author, description, kind: "script" or "program", permissions, files,
// bytes, installed (one of that name is there already), source}. A program
// plugin's one permission is "program": it runs as a program of its own.
//
// It fails for a package with no plugin.json, one that can't be read or
// names neither a script nor a command, a permission this Omabook doesn't
// know, a file that would land outside the plugin's folder ("../", an
// absolute path, a link), and one that is over the limits.
QJsonObject inspect(const Context &context, const QString &source, const InstallOptions &options = {});
// Installs it into the plugin folder: the same answer, with {id, folder,
// enabled, replaced}. The plugin is switched on or off before it is put in
// place, so one that wasn't approved never runs.
QJsonObject install(const Context &context, const QString &source, const InstallOptions &options = {});
// Moves a plugin of the user's own to the trash (or deletes it where there
// is no trash): {name, id, folder, trashed}. Built-in plugins and Calibre's
// are not in the plugin folder and can only be switched off.
QJsonObject remove(const Context &context, const QString &name);

} // namespace PluginTools
