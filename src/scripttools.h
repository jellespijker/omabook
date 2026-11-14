#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <functional>

class Library;
class Settings;

// What an agent does with script plugins, each as one function that answers
// in JSON: the `omabook plugin` commands print these, and the MCP tools
// (plugin_api, plugin_list, plugin_write, plugin_check, plugin_run,
// plugin_enable) are the same functions. None needs the window; a plugin is
// loaded into an engine of its own for the length of the call.
//
// Every answer is an object. When something went wrong it has an "error"
// (English, with file and line when the script is at fault) and the call
// failed; otherwise it didn't.
namespace ScriptTools {

struct Context {
    QString folder;                     // the plugin folder; the user's when empty
    Settings *settings = nullptr;       // the settings; the user's when null
    std::function<Library *()> library; // the library; the one the settings name when not given
    double waitMinutes = 10;            // how long `run` waits for an answer before it gives up
};

// The host API: ScriptApi::json(), with the Markdown beside it.
QJsonObject api();
// {"folder", "plugins": [...]}: every script plugin, what it registered,
// whether it runs and what is wrong with it if anything is.
QJsonObject list(const Context &context);
// Starts a plugin from a template (see ScriptApi::templates()).
QJsonObject create(const Context &context, const QString &name, const QString &templateName = "hook");
// Writes one file of a plugin (plugin.json, a script, a panel), making the
// plugin's folder if need be, and checks the plugin when it is whole.
QJsonObject write(const Context &context, const QString &name, const QString &file, const QString &contents);
// Loads the plugin with no window: {"ok", "errors": [{file, line, message}],
// "registered": {...}, "permissions", "log"}.
QJsonObject check(const Context &context, const QString &name);
// Runs one thing of a plugin's and waits for its answer: a top-level
// function, or "metadata:<source>", "command:<id>", "catalog:<format>",
// "importer:<name>", "event:<name>". {"result", "said", "log"}.
QJsonObject run(const Context &context, const QString &name, const QString &what, const QJsonValue &arguments = {});
QJsonObject enable(const Context &context, const QString &name, bool enabled);

bool failed(const QJsonObject &answer);
// Whether a name can be a plugin's folder: lower-case letters, digits, - and _.
bool validName(const QString &name);
// "main.js:12: TypeError: ..." as {file, line, message}.
QJsonObject located(const QString &error);

} // namespace ScriptTools

// `omabook plugin ...`: see cli.cpp.
int runPluginCli(const QStringList &arguments);
