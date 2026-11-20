#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "graph.h"
#include "graphbuilder.h"
#include "library.h"

// What the command line and the MCP server do with the knowledge graph, each
// written once and answering in JSON: `omabook graph ...` prints these, and
// the graph_* tools are those commands. None needs the window.
//
// Every answer is an object. When something went wrong it has an "error"
// (English) and the call failed; otherwise it didn't.
namespace GraphOps {

bool failed(const QJsonObject &answer);

// A place in a book as it is named on the command line: a section, with how
// far into it (the whole of it when not said).
std::optional<GraphPlace> place(qint64 bookId, const QString &section, const QString &fraction = {});

// {"nodes", "edges"}: the graph of a book (up to a place, when given), or
// without a book a whole scope, the most connected things first.
// With `beyond`, a book's graph comes with what it connects to elsewhere,
// as "beyond": {"nodes", "edges"}: things of other books tied to it by a
// connection across books, each with the books it is "in", and the other
// books that share a thing with it.
QJsonObject show(Library &library, qint64 bookId, const QString &scope = {}, const std::optional<GraphPlace> &upTo = std::nullopt,
                 int limit = 500, bool beyond = false);
// Connects a book to the other books read, with one request to the model:
// {"book", "asked", "links", "topics", "skipped", "connections": [...]}.
// With `estimate` nothing is sent: {"book", "requests", "own", "others"}.
QJsonObject connect(Library &library, qint64 bookId, const GraphBuild::Asker &ask, bool again = false, bool estimate = false);
// {"nodes"}: the things whose name, other names or summary hold every word.
// With no scope the general graph is searched; `everywhere` searches all.
QJsonObject find(const Library &library, const QString &words, const QString &kind = {}, const QString &scope = {},
                 bool everywhere = false, int limit = 50);
// {"node", "seen", "nodes", "edges"}: one thing, the places it was met at,
// and what is within `depth` relations of it. With a book and a place, as a
// reader there knows it: its summary and names then, and only what was met
// by then around it. With `timeline`, also "timeline": when each thing known
// of it was revealed ({what, text, revealed: {book, section, fraction,
// anchor}}), up to that place when one is given, each past its reader's
// furthest place in its book marked "ahead".
QJsonObject node(const Library &library, qint64 id, int depth = 1, bool timeline = false, qint64 bookId = 0,
                 const std::optional<GraphPlace> &upTo = std::nullopt);
// Adds a thing, or brings the one of that name up to date: {"kind", "name",
// "summary", "aliases", "data", "scope", "book", "section", "fraction"}.
// With a book and no section it is placed where the book was read to.
QJsonObject add(Library &library, const QJsonObject &thing);
QJsonObject link(Library &library, qint64 source, qint64 target, const QString &relation, const QString &note = {},
                 const std::optional<GraphPlace> &learned = std::nullopt);
QJsonObject unlink(Library &library, qint64 edge);
QJsonObject merge(Library &library, qint64 keep, qint64 other);
QJsonObject remove(Library &library, qint64 id);

// Brings the graph of a book up to a place with the model, in as many
// requests as it takes: {"requests", "nodes", "edges", "sent", "complete",
// "up_to", "chronology" (GraphBuild::Timed), "problems"}. Without a place it goes as far as the book was
// read; `whole` takes the book to its end, read or not; `again` starts
// over from the first page.
QJsonObject build(Library &library, qint64 bookId, const std::optional<GraphPlace> &upTo, bool whole, bool again,
                  const GraphBuild::Asker &ask, const std::function<void(const GraphBuild::Built &)> &said = {});

// {"format", "content"}: a book's graph or a scope as a file other tools
// read; written to `output` instead when that is given.
QJsonObject exported(Library &library, qint64 bookId, const QString &scope, const std::optional<GraphPlace> &upTo,
                     const QString &format, const QString &output = {});

QJsonObject scopes(const Library &library);
QJsonObject copyScope(Library &library, const QString &from, const QString &to);
QJsonObject removeScope(Library &library, const QString &scope);
// {"days": [{"book", "title", "day", "from", "to"}]}: what was read when.
QJsonObject log(const Library &library, qint64 bookId = 0);
// Brings the books' own nodes (status, stars, authors, series, tags) in
// step with the library.
QJsonObject sync(Library &library);

} // namespace GraphOps

// `omabook graph ...`: see cli.cpp.
int runGraphCli(const QStringList &arguments);
