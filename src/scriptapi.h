#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// What a script plugin can do, as one table. The `omabook` object a script
// sees is built in scriptprelude.js; this table is what describes it: the
// reference `omabook plugin api` prints, the "Writing a plugin" part of
// AGENTS.md and of the agent skill, and the tool an agent asks over MCP all
// come from here, and a test holds the table and the live object to each
// other, so the instructions can't drift from what a script can really call.
namespace ScriptApi {

struct Entry {
    QString name;        // "library.setMeta", without the leading "omabook."
    QString signature;   // "library.setMeta(id, details)"
    QString permission;  // the permission plugin.json must declare, or empty
    QString what;        // what it does and answers, in English
    bool value = false;  // a value to read rather than a function to call
};

struct Event {
    QString name;        // "book.added"
    QString payload;     // the object a handler is given, as it would be written
    QString what;
};

struct Permission {
    QString name;        // "http"
    QString what;
};

const QList<Entry> &entries();
const QList<Event> &events();
const QList<Permission> &permissions();
QStringList eventNames();
QStringList permissionNames();
// Where a command or a panel can be put.
QStringList commandPlaces();
QStringList panelPlaces();

// The whole reference as Markdown, and as JSON for `--json` and MCP.
QString markdown();
QJsonObject json();
// "Writing a plugin": the layout, the loop, the reference and two worked
// examples (the templates themselves), for AGENTS.md and the skill.
QString guide();

// The plugins `omabook plugin new` starts from, compiled into the binary.
QStringList templates();
// The files of a template by name, with "{{name}}" for the plugin's name.
QStringList templateFiles(const QString &name);
QByteArray templateFile(const QString &name, const QString &file);

} // namespace ScriptApi
