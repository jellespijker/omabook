#pragma once

#include <QString>
#include <QStringList>
#include <expected>
#include <optional>

#include "graph.h"
#include "graphlayers.h"
#include "library.h"

// What the AI is told from the knowledge graph, and what is written into it
// of what the reader asked. Everything here is bounded: a slice of a book's
// graph around what a question touches, never the graph; a picture of the
// library in a few thousand characters, whatever the library's size. Each
// is read in one tenant (ADR 0003), the graph's it is given: the AI is never
// told anything of another.
namespace GraphContext {

// Whether words (already lower case) name a thing: by its name or one of
// its other names, or with `loosely` by any longer word of them, since a
// story says "Mara" far more often than "Mara Venn".
bool namedIn(const GraphNode &node, const QString &lower, bool loosely = false);

// How far a book's reader has got, which bounds what may be said of the
// book elsewhere: all of it for a book marked as read, up to its reading
// place for one being read, and nothing of a book not begun, however far
// its graph was built.
struct Reach {
    bool any = false, all = false;
    GraphPlace place;
    bool covers(const GraphPlace &at) const { return all || (any && at.notAfter(place)); }
    // The bound for Graph::ofBook: none for a book read to its end.
    std::optional<GraphPlace> upTo() const { return all ? std::nullopt : std::optional(place); }
};
Reach reachOf(const Library &library, qint64 bookId);

// The furthest a book was ever read: where its reader is, or further on
// when they went back to an earlier page.
GraphPlace furthest(const Library &library, qint64 bookId);
// Past where, in a book, what is known of it gives away what happens: the
// furthest its reader got. Nothing for a book marked as read, or never
// opened here (read elsewhere, its graph built whole on request), which has
// no spoilers.
std::optional<GraphPlace> spoilersAfter(const Library &library, qint64 bookId);
// Whether a place is behind the reader of its book, as far as they ever
// read it; a place of no book always is. readerKnows goes by reachOf, what
// the AI may be told (nothing of a book not begun); unspoiled goes by
// spoilersAfter, what may be shown without a warning.
GraphKnown readerKnows(const Library &library);
GraphKnown unspoiled(const Library &library);

// The graphs of the spaces this library subscribes to, as its reader knows
// them (#5): the one cut every surface shows or tells a layer by. Of each
// book, what was met or learned no further than its reader's own place in it
// (reachOf: all of a book read, nothing of one not begun or only wished
// for), with `upTo` of its book no further than that place either; never
// spoilersAfter's "no spoilers in a book never opened here", and never
// more for a place asked for further on. Each layer stays apart from the
// reader's own graph (GraphLayers), and holds only what it has of the books
// of the graph's tenant.
QList<GraphLayers::Layer> layers(const Graph &graph, const std::optional<GraphPlace> &upTo = std::nullopt);

// A book's graph up to a place without what isn't of the story: the
// questions asked there, and the book itself.
GraphView story(const Graph &graph, qint64 bookId, const std::optional<GraphPlace> &upTo = std::nullopt);

// The part of a book's graph a question touches, up to a place and no
// further: the things named in `words` (the question, and what the reader
// selected), then their neighbours, each with what is known of it and how
// they relate. Empty when the words name nothing known.
QString forQuestion(const Graph &graph, qint64 bookId, const GraphPlace &upTo, const QString &words, int mostNodes = 24);

// What the graph holds of a book up to a place, of these kinds, one thing
// to a line with its relations: the people for a who-is-who, the threads
// and events too for a catch-up.
QString digest(const Graph &graph, qint64 bookId, const GraphPlace &upTo, const QStringList &kinds, int mostNodes = 40);

// The graph across the library, for a question asked of all of it and for
// suggestions: what was read when, with its stars; the themes, ideas and
// real-world topics met most, with the books they were met in; what in the
// graph the question's words name; and the latest questions asked. At most
// `room` characters. Of the graph's tenant only: its books, what they
// taught, and what was asked in it (graphpicture.cpp).
QString picture(const Graph &graph, const QString &question = {}, qsizetype room = 6000);
// The same in the tenant the command line and the MCP server name
// (Tenancy::chosen: a collection's, or the library's only one), or why not;
// nothing at all when it isn't `wanted` (the graph is off).
std::expected<QString, QString> pictureIn(const Library &library, const QString &collection, const QString &question = {},
                                          bool wanted = true);

// Writes a question into the graph as a "question" node: its words as the
// name, the gist of the answer as the summary, tied to the book it was asked
// of (none for one asked of the library) at the place it was asked, and to
// the things the question and the answer name, of the graph's tenant, and
// kept in that tenant whatever books it names. Answers the node.
qint64 noteQuestion(const Graph &graph, qint64 bookId, const std::optional<GraphPlace> &place, const QString &question,
                    const QString &answer);

// What the parts of the context write alike: a thing on one line, with its
// kind, other names and summary; a text cut at the end of a sentence; and
// the book a thing was met in, other than `notBook`, of the graph's tenant.
QString line(const GraphNode &node);
QString brief(const QString &text, qsizetype room);
QString bookOf(const Graph &graph, qint64 node, qint64 notBook);

} // namespace GraphContext
