#pragma once

#include <QJsonObject>

#include "library.h"

// The knowledge graph, carried between two computers' libraries by a sync.
//
// A graph names a book by its number in its own library, and the same book
// has another number on another computer. So what travels names each book as
// a sync knows it on both sides: by its files' hashes, or for a book without
// a file by its entry keys (Library::entryKeys). A thing is the same thing
// when it is of the same kind and goes by the same name, or one of its other
// names, as Graph::named finds it.
//
// Taking is a merge, never an overwrite: both computers may have learned
// something of the same book. What arrives keeps the moment it was revealed,
// and of the same fact the earlier moment stands, so the graph a reader sees
// is never ahead of where they are on either computer. Only what is missing
// is written, so a second sync changes nothing.
//
// Only the general graph travels. The things that stand for books, authors,
// series and tags are made from the library itself (Graph::bookNode), and a
// made-up world (a scope such as "world:42") belongs to the story it is of.
namespace GraphSync {

// The general graph, as the other side of a sync reads it.
QJsonObject bundle(const Library &library);

// Takes in another computer's bundle: answers how many things, facts,
// places, relations and reading days were new here. For an Omabook from
// before the change log; between two that speak it the graph travels by the
// log (synctakegraph.h).
int take(Library &library, const QJsonObject &bundle);

} // namespace GraphSync
