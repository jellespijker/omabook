#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>
#include <utility>

#include "library.h"

// What is known, as a graph kept in the library's own database: the people,
// places, groups, ideas (a magic system, a technology), events and plot
// threads of the books being read; the books, authors, series and tags
// themselves; the questions that were asked; and the topics beyond the books
// that they touch. Nodes are things, edges say how two things relate.
//
// Every fact remembers where it was learned: which book, and how far into
// it. The graph of a book is therefore never ahead of its reader, and asking
// for it "up to" a place leaves out what comes after.
//
// A scope is a graph of its own. The empty scope is the general graph, of
// everything read. A made-up world (a story being written) keeps its own
// under a name such as "world:42", apart from what real books say.

// A place in a book: where a thing was first met or a fact was learned.
struct GraphPlace {
    qint64 bookId = 0;
    int section = 0;
    double fraction = 0;

    // Whether this place is at or before `other`, in the same book.
    constexpr bool notAfter(const GraphPlace &other) const noexcept {
        return std::pair{section, fraction} <= std::pair{other.section, other.fraction};
    }
};

struct GraphNode {
    qint64 id = 0;
    QString scope;      // "" for the general graph
    // What kind of thing: "character", "place", "group", "concept", "item",
    // "event", "thread" (a plot line), "theme", "topic" (knowledge beyond
    // the book), "question", "book", "author", "series", "tag". Others are
    // allowed; these are the ones Omabook itself makes and draws.
    QString kind;
    QString name;
    QString summary;    // what is known of it, in a sentence or a few
    // Anything else: {"aliases": ["Strider"], "url": "…", "asked": "…"}.
    QJsonObject data;
    qint64 bookId = 0;  // for a "book" node, the book it stands for
    qint64 updated = 0;

    QStringList aliases() const;
    QJsonObject toJson() const;
};

struct GraphEdge {
    qint64 id = 0;
    qint64 source = 0, target = 0;
    // How the source relates to the target, as a short verb phrase:
    // "mentor of", "rules", "part of", "wrote", "asked about".
    QString kind;
    QString note;
    GraphPlace learned; // bookId 0 when it comes from no book

    // A connection across books: between a thing of one book and a thing of
    // another (or a real-world work both point at), noticed while reading
    // and not said by either book. Such an edge is told from a book's own
    // facts by its kind, which starts with this mark: "across: echoes".
    static QString acrossMark() { return QStringLiteral("across: "); }
    bool across() const { return kind.startsWith(acrossMark()); }
    // The kind without that mark: "echoes".
    QString relation() const { return across() ? kind.mid(acrossMark().size()) : kind; }

    QJsonObject toJson() const;
};

// Some nodes and the edges among them.
struct GraphView {
    QList<GraphNode> nodes;
    QList<GraphEdge> edges;

    QJsonObject toJson() const;
    // As a file other tools read: "json", "graphml", "dot" (Graphviz) or
    // "cypher" (statements that build it in Neo4j). Null for another format.
    QByteArray exported(const QString &format) const;
    static QStringList formats() { return {"json", "graphml", "dot", "cypher"}; }
};

// One day's reading of a book.
struct ReadingDay {
    qint64 bookId = 0;
    QString day;            // ISO date
    double from = 0, to = 0; // percent of the book
};

// The graph of one library. It works on the library's connection, so it
// lives as long as, and on the thread of, the Library it was made from.
class Graph {
public:
    explicit Graph(const Library &library);

    // Adds a thing, or brings the one already known by that name (or one of
    // its aliases) in the same scope and of the same kind up to date: a
    // summary given replaces the old, data is merged, aliases are added to.
    // `seen` is where it was met; the earliest place per book is kept.
    qint64 upsert(const QString &scope, const QString &kind, const QString &name, const QString &summary = {},
                  const QJsonObject &data = {}, const std::optional<GraphPlace> &seen = std::nullopt);
    // Relates two things; saying the same again only updates the note, and
    // keeps the earlier place it was learned at.
    qint64 link(qint64 source, qint64 target, const QString &kind, const QString &note = {},
                const std::optional<GraphPlace> &learned = std::nullopt);
    bool remove(qint64 node);
    bool unlink(qint64 edge);
    // Folds one node into another: its edges, places and aliases move over.
    bool merge(qint64 keep, qint64 other);

    // Changes a node by hand: what it is called and what is said of it. The
    // old name stays as one of its names, so what is read later still finds
    // it. False when another node of its kind already goes by the new name.
    bool edit(qint64 node, const QString &name, const QString &summary);

    std::optional<GraphNode> node(qint64 id) const;
    // Where a node was met: its earliest place in each book it is in.
    QList<GraphPlace> seen(qint64 node) const;
    // The node known by a name or alias; 0 when there is none.
    qint64 named(const QString &scope, const QString &kind, const QString &name) const;
    // Nodes whose name, aliases or summary hold every word; `kind` and
    // `scope` narrow it when given (scope: nothing for every scope).
    QList<GraphNode> find(const QString &words, const QString &kind = {}, const std::optional<QString> &scope = QString(),
                          int limit = 50) const;

    // A node and what is within `depth` edges of it.
    GraphView around(qint64 node, int depth = 1, int limit = 200) const;
    // What a book's reader has met: the nodes seen in it and the edges learned
    // in it, with `upTo` only those at or before that place. It is the
    // general graph's unless a scope is named: a world copied from what the
    // books say keeps where its things were met, and is not the book's graph.
    GraphView ofBook(qint64 bookId, const std::optional<GraphPlace> &upTo = std::nullopt, const QString &scope = {}) const;
    // The things met in a book, in the order its reader first met them, each
    // with that place. What a reader at some place has met is therefore the
    // front of this list, up to the first thing met after it.
    QList<std::pair<GraphNode, GraphPlace>> metIn(qint64 bookId, const QString &scope = {}) const;
    // What a book connects to elsewhere: the things of other books that a
    // connection across books (see GraphEdge::across) ties to this book's
    // things, and the other books that a thing of this book is also in (a
    // theme two books share), as those books' own nodes. The edges are the
    // ties themselves: each has one end among ofBook(bookId, upTo).nodes and
    // the other among the nodes answered here. With `upTo`, only this
    // book's things met up to that place, and only the connections noticed
    // in this book up to it.
    GraphView beyond(qint64 bookId, const std::optional<GraphPlace> &upTo = std::nullopt) const;
    // The connections across books there are, the latest first, with the
    // things at both their ends.
    GraphView acrossBooks(int limit = 50) const;
    // A whole scope, the most connected nodes first when it is cut at `limit`.
    GraphView whole(const QString &scope = {}, int limit = 2000) const;

    // The node standing for a book, made (with its author, series and tags,
    // and the edges between them) when it isn't there yet and brought up to
    // date when it is. 0 for a book the library doesn't have.
    qint64 bookNode(qint64 bookId);
    // Does that for every book, and forgets the nodes of books that are gone.
    void syncLibrary();

    // Scopes: the graphs there are, and copying one whole (a world forked).
    QStringList scopes() const;
    bool copyScope(const QString &from, const QString &to);
    bool removeScope(const QString &scope);

    // The reading log: that a book went from one percent to another today.
    void logReading(qint64 bookId, double from, double to, const QString &day = {});
    QList<ReadingDay> readingLog(qint64 bookId = 0) const;

private:
    [[nodiscard]] GraphView collect(const QString &nodesWhere, const QVariantList &values, int limit) const;
    [[nodiscard]] QList<GraphEdge> edgesAmong(const QList<GraphNode> &nodes) const;

    const Library *m_library;
};
