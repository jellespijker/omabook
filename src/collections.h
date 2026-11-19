#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include "library.h"

// Collections (library.h) as the command line, the MCP tools and a sync
// see them. The window has its own side of them in Backend.
namespace Collections {

// Each collection with how many books it holds now: {"id", "name", "path", "parent", "depth", "rule",
// "count", "included", "excluded", "shelf", "format"}.
QJsonArray list(const Library &library);
// One collection and its books, as `omabook list` gives them.
QJsonObject show(const Library &library, const Collection &collection);

// What a sync with an Omabook from before the change log carries of the
// collections (between two that speak it they travel by the log, synctake.h):
// everything of each, the removed ones too, with its books named by their
// files' hashes, which is how two computers know a book is the same one.
QJsonArray manifest(const Library &library);
// Takes collections from another computer's manifest: a collection it has
// that this library lacks is added, and of one both have the one changed
// last wins, its books and its removal included. Books this library doesn't
// hold are left out. How many collections changed here.
int take(Library &library, const QJsonArray &theirs);

// `omabook collections [list|add|rule|rename|move|include|exclude|follow|remove|show|format|sort|take|share] …`
int run(const QStringList &arguments);

} // namespace Collections
