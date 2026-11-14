#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class Library;

// Omabook as a Model Context Protocol server (`omabook mcp`): the library and
// everything the command line does with it, as tools another agent calls over
// standard input and output. It needs no display and works on the same
// library as the window, at the same time.
//
// Every tool lives in one table. To add one from another part of Omabook,
// register it in that part's own .cpp, at file scope:
//
//     #include "mcp.h"
//
//     static const bool registered = Mcp::registerTool({
//         "calibre_find",                                  // name: snake_case, starting with its area
//         "Find Calibre libraries",                        // title, for people
//         "Lists the Calibre libraries on this computer.", // description, for the agent
//         Mcp::schema({{"folder", Mcp::string("Where to look; the home folder when not given.")}}),
//         Mcp::ReadOnly,
//         [](const QJsonObject &arguments) {
//             return Mcp::Result::ok({{"libraries", QJsonArray{}}});
//         },
//     });
//
// That is all: the tool is listed and callable. (The registration runs before
// main(), so the handler must not touch anything at registration time, and
// the .cpp must be one the program already uses, which every file in src/ is.)
namespace Mcp {

// What a tool does to the world, as the protocol's annotations say it.
enum Effect {
    ReadOnly,    // changes nothing
    Changes,     // adds or changes something, and can be done again or undone
    Destructive, // takes something away, or overwrites what can't be put back
};

// What a tool answers: an object of results, or why it could not.
struct Result {
    QJsonObject data;
    QString error; // English; not empty when the tool failed

    static Result ok(const QJsonObject &data = {}) { return {data, {}}; }
    static Result fail(const QString &why) { return {{}, why}; }
    bool failed() const { return !error.isEmpty(); }
};

using Handler = std::function<Result(const QJsonObject &arguments)>;

struct Tool {
    QString name;
    QString title;
    QString description;
    QJsonObject inputSchema; // a JSON Schema object; see schema() below
    Effect effect = ReadOnly;
    Handler handler;
    // Whether it reaches beyond this computer (a catalog, a model, ssh).
    bool openWorld = false;
};

// Adds a tool to the table. False, with a line on standard error, for a tool
// without a name or handler or with a name already taken.
bool registerTool(const Tool &tool);
// Runs one of Omabook's own commands (`omabook <arguments> --json`) on the
// library in use and hands back what it printed: the way a tool is made of
// something the command line already does, so the two can't drift apart.
// With `json` false the command's plain output comes back as {"output": …}.
Result command(const QStringList &arguments, bool json = true);
// Every registered tool, by name.
QList<Tool> tools();

// Building an input schema without writing the JSON out by hand.
// schema({{"id", integer("The book's number.")}, ...}, {"id"}) is an object
// with those properties, of which "id" is required, and no others allowed.
QJsonObject schema(std::initializer_list<std::pair<QString, QJsonValue>> properties = {}, const QStringList &required = {});
QJsonObject string(const QString &description);
QJsonObject integer(const QString &description, int minimum = 0);
QJsonObject number(const QString &description, double minimum, double maximum);
QJsonObject boolean(const QString &description);
QJsonObject choice(const QString &description, const QStringList &values);
QJsonObject list(const QString &description, const QJsonObject &items);
// An object whose shape is the tool's own business.
QJsonObject object(const QString &description);

// The library a tool works on: the one the server was started for
// (`omabook mcp --library <folder>`), else the one the window has open, which
// is followed when it is switched. Null, with `error`, when it can't be opened.
std::shared_ptr<Library> library(QString *error);
// A book's number from the arguments, checked against the library; 0 with
// `error` when it is missing or there is no such book.
qint64 bookId(const Library &library, const QJsonObject &arguments, QString *error, const QString &key = "id");

// One request or notification, as a line of JSON; the answer as a line of
// JSON, or nothing for a notification. This is the whole protocol, apart
// from reading and writing the lines, so tests can call it directly.
QByteArray handle(const QByteArray &line);

// `omabook mcp` and `omabook mcp install`: see cli.cpp.
int run(const QStringList &arguments);

} // namespace Mcp
