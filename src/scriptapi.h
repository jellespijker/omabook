#pragma once

#include <QJsonObject>
#include <QJsonValue>
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
// The parts of entries() after the library's: what a plugin puts in the window
// (scriptapiui.cpp), the knowledge graph's and what goes into spaces
// (scriptapigraph.cpp), and the rest.
QList<Entry> uiEntries();
// What of the window reaches the person's files and clipboard (scriptapiui.cpp,
// #131), and what a plugin exports for them: a file saved, a PDF (#138).
QList<Entry> deskEntries();
QList<Entry> graphEntries();
QList<Entry> spaceEntries();
QList<Entry> laterEntries();
// Asking the model and recognising ink and speech (scriptapiassist.cpp).
QList<Entry> assistEntries();
// What a plugin writes beside a book being read: the reader's selection,
// passages, and books by their global id (scriptapireading.cpp, #137).
QList<Entry> readingEntries();
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
// Its parts on what a plugin makes and keeps (scriptapiguide.cpp): books of
// its own, and files, the person's and the clipboard among them, and what
// travels to their other computers (#131).
QString booksGuide();
QString filesGuide();
QString exportsGuide();
// And on writing beside a book being read, and recording beside it (#137, #135).
QString besideGuide();

// The version of this Omabook, which a plugin's plugin.json may ask for at
// least: {"omabook": "0.1.0"}. A plugin that asks for a newer one is not
// loaded and not run.
QString hostVersion();
// Why a plugin that asks for `wanted` can't be used here, in English; empty
// when it can (it asks for this version or an older one, or doesn't say).
// `malformed` when what it asks for is no version at all.
QString needsNewer(const QJsonValue &wanted, bool *malformed = nullptr);

// The plugins `omabook plugin new` starts from, compiled into the binary.
QStringList templates();
// The files of a template by name, with "{{name}}" for the plugin's name.
QStringList templateFiles(const QString &name);
QByteArray templateFile(const QString &name, const QString &file);

} // namespace ScriptApi
