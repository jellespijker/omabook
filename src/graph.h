#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
#include <utility>

#include "bookplace.h"
#include "eventtime.h"
#include "library.h"
#include "tenancy.h"

// What is known, as a graph kept in the library's own database: the people,
// places, groups, ideas (a magic system, a technology), events and plot
// threads of the books being read; the books, authors, series and tags
// themselves; the questions that were asked; and the topics beyond the books
// that they touch. Nodes are things, edges say how two things relate.
//
// Every fact remembers where it was learned: which book, and how far into
// it, with a few words of the text there to find the spot again. That goes
// for what is said of a thing too: each summary it had, and each name it was
// given, is kept with the moment it was revealed. The graph of a book is
// therefore never ahead of its reader, and asking for it "up to" a place
// leaves out what comes after, down to a summary that changed later.
//
// A scope is a graph of its own. The empty scope is the general graph, of
// everything read. A made-up world (a story being written) keeps its own
// under a name such as "world:42", apart from what real books say.

// A place in one of the library's books: where a thing was first met or a
// fact was learned, and the place the graph is cut at for its reader.
struct GraphPlace : BookPlace {
    qint64 bookId = 0;

    constexpr GraphPlace() = default;
    constexpr GraphPlace(qint64 bookId, BookPlace place) : BookPlace{place}, bookId{bookId} {}
    constexpr GraphPlace(qint64 bookId, int section, double fraction) : GraphPlace{bookId, BookPlace{section, fraction}} {}

    // Two places are the same in the same book only.
    constexpr bool operator==(const GraphPlace &) const = default;

    // The place in its book, whichever book that is.
    [[nodiscard]] constexpr const BookPlace &inBook() const noexcept { return *this; }
    // Whether this place is at or before `other`, in the same book.
    [[nodiscard]] constexpr bool notAfter(const GraphPlace &other) const noexcept { return inBook() <= other.inBook(); }
};

// A moment of a book: a place, and a few words of the text there (the
// sentence a fact came from, cut short) that find the very spot again.
// Only the words needed to find it are kept, never more of the text.
struct GraphMoment {
    GraphPlace place;
    QString anchor;
};

// One thing learned of a node, with the moment it was revealed: what is
// said of it ("summary"), another name it goes by ("alias"), what a tenant
// calls it, renamed there by hand ("name", of no book, #110), or for an
// event when it happens in its world ("when", chronology.h), or the
// reader's word on a timeline that branches ("branch", chronologybranch.h). A node's
// summary changes as its book goes on, and each version is kept here, so
// what was known at any place is known later too. A fact of no book (put
// in by hand, or from another source) has a place whose bookId is 0.
struct GraphFact {
    qint64 id = 0;
    qint64 node = 0;
    QString kind;  // "summary", "alias", "name", "when" or "branch"
    QString value; // for "when", an EventTime as it is kept (EventTime::written)
    GraphMoment revealed;

    // What a fact of `kind` keeps of `value`: a name on one line, a time as
    // EventTime writes it (nothing when it is no time), the rest trimmed.
    [[nodiscard]] static QString kept(const QString &kind, const QString &value) {
        if (kind == "alias" || kind == "name")
            return value.simplified();
        if (kind == "when")
            return EventTime::kept(value);
        return value.trimmed();
    }
    // The time a "when" fact says; nothing for another fact.
    [[nodiscard]] std::optional<EventTime> when() const {
        if (kind != "when")
            return std::nullopt;
        const auto time = EventTime::parse(value);
        return time ? std::optional(*time) : std::nullopt;
    }

    QJsonObject toJson() const;
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
    // The kinds of thing a book's story is told in, which the AI reads into
    // the graph and is told from it (Prompts::graphKinds).
    static QStringList storyKinds() {
        return {"character", "place", "group", "concept", "item", "event", "thread", "theme", "topic", "timeline"};
    }
};

struct GraphEdge {
    qint64 id = 0;
    qint64 source = 0, target = 0;
    // How the source relates to the target, as a short verb phrase:
    // "mentor of", "rules", "part of", "wrote", "asked about".
    QString kind;
    QString note;
    GraphPlace learned; // bookId 0 when it comes from no book
    QString anchor;     // a few words of the text where it was learned, if known

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

// One step of a node's timeline: something revealed of it, and where.
struct GraphReveal {
    // "met" (where it first shows in a book), "summary", "alias" or "relation".
    QString what;
    // The summary or the name; for a relation, how it reads from the node
    // ("mentor of", with `other` at its far end, or `other` first when the
    // relation points at the node).
    QString text;
    GraphMoment revealed;
    qint64 edge = 0, other = 0;
    QString otherName;
    bool outgoing = true;

    QJsonObject toJson() const;
};

// Whether what was revealed at a place is known to whoever asks: a reader
// at some place of a book, say, knows what came before it.
using GraphKnown = std::function<bool(const GraphPlace &)>;

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
    qint64 seconds = 0;      // how long it took, the time away left out (ReadingLog); 0 when not known
};

// The graph of one library. It works on the library's connection, so it
// lives as long as, and on the thread of, the Library it was made from.
class Graph {
public:
    explicit Graph(const Library &library);
    // The graph as one tenant sees it (ADR 0003): what its readers keep to,
    // with within(), so that nothing of another tenant is read (#95).
    [[nodiscard]] static Graph forTenant(const Library &library, Tenant tenant) {
        Graph graph(library);
        graph.m_tenant = std::move(tenant);
        return graph;
    }
    // The graph a reader that names no tenant reads (Tenancy::preferred).
    [[nodiscard]] static Graph ofLibrary(const Library &library) { return forTenant(library, Tenancy::preferred(library)); }
    // The graph a thing is read in: the first of its tenants, or for one of
    // none (what every tenant sees) Tenancy::preferred's.
    [[nodiscard]] static Graph ofThing(const Library &library, qint64 node) {
        const QList<Tenant> all = Tenancy::tenants(library);
        for (const Tenant &tenant : Tenancy::ofNode(library, node))
            if (all.contains(tenant))
                return forTenant(library, tenant);
        return ofLibrary(library);
    }
    // The graph a scope is read in: a world (a scope of its own) whole, of
    // no tenant, whatever tenants the books its things were met in are in (a
    // sequel's world is met in its first story's book too, #112); the general
    // graph, Tenancy::preferred's.
    [[nodiscard]] static Graph ofScope(const Library &library, const QString &scope) {
        return scope.isEmpty() ? ofLibrary(library) : Graph(library);
    }
    // The graph read from a collection's view: its tenant's, or for none (the
    // whole shelf) Tenancy::preferred's, until the window says which (#96).
    [[nodiscard]] static Graph ofCollection(const Library &library, qint64 collectionId) {
        return forTenant(library, Tenancy::ofCollection(library, collectionId).value_or(Tenancy::preferred(library)));
    }
    // The graph a reader of a book reads, in the book's tenant (Tenancy::ofReader).
    [[nodiscard]] static Graph ofReader(const Library &library, qint64 bookId) {
        return forTenant(library, Tenancy::ofReader(library, bookId));
    }
    [[nodiscard]] const std::optional<Tenant> &tenant() const noexcept { return m_tenant; }
    [[nodiscard]] const Library &library() const noexcept { return *m_library; }
    // The condition that keeps a query to the tenant (TenantFilter); none
    // for the graph of no tenant, which reads all of it: what writes the
    // graph, keeps it and carries it to other computers, never a reader.
    [[nodiscard]] const std::optional<TenantFilter> &within() const {
        if (m_tenant && !m_within)
            m_within = Tenancy::filter(*m_library, *m_tenant);
        return m_within;
    }
    // Whether a book is in the tenant, and so is the graph's to read and
    // tell of; every book is the graph of no tenant's.
    [[nodiscard]] bool sees(qint64 bookId) const;
    // Whether two books share a tenant (as BookSides places them): what one
    // book's graph, as it is built, may draw on of the other's.
    [[nodiscard]] bool together(qint64 bookId, qint64 otherBook) const;

    // Adds a thing, or brings the one already known by that name (or one of
    // its aliases) in the same scope and of the same kind up to date: a
    // summary given replaces the old, data is merged, aliases are added to.
    // `seen` is where it was met, with `anchor` the words there that name it;
    // the earliest place per book is kept. The summary and the names new to
    // it are kept as facts (see facts()) revealed at `revealed`, or where it
    // was met when that isn't given. Put in by hand in the general graph of a
    // tenant, met in no book, it is kept in that tenant (Tenancy::madeInKey).
    qint64 upsert(const QString &scope, const QString &kind, const QString &name, const QString &summary = {},
                  const QJsonObject &data = {}, const std::optional<GraphPlace> &seen = std::nullopt,
                  const QString &anchor = {}, const std::optional<GraphMoment> &revealed = std::nullopt);
    // Relates two things; saying the same again only updates the note, and
    // keeps the earlier place it was learned at in its book (with its
    // anchor). Said in another book, it keeps that book's note beside its
    // own; written by hand in a tenant, that tenant's (#109).
    qint64 link(qint64 source, qint64 target, const QString &kind, const QString &note = {},
                const std::optional<GraphPlace> &learned = std::nullopt, const QString &anchor = {});
    // Keeps a fact of a node: a summary (which becomes its summary now),
    // another name (which it goes by from now on), a time (kept as
    // EventTime writes it, and only when it is one) or a word on a branch
    // (chronologybranch.h), revealed at a moment, or
    // at none. The same fact revealed again in the same book keeps the
    // earlier moment. Answers the fact's id, 0 when nothing was kept.
    qint64 learn(qint64 node, const QString &kind, const QString &value, const std::optional<GraphMoment> &revealed = std::nullopt);
    bool remove(qint64 node);
    bool unlink(qint64 edge);
    // Takes away where a thing was met in a book (a sighting, as upsert's
    // `seen` made it), of a book the tenant sees; false when there was none.
    // It goes to one's other computers as a sighting does (#136).
    bool unsee(qint64 node, qint64 bookId);
    // Folds one node into another: its edges, places and aliases move over.
    bool merge(qint64 keep, qint64 other);

    // Changes a node by hand: what it is called and what is said of it. The
    // old name stays as one of its names, so what is read later still finds
    // it. False when another node of its kind already goes by the new name.
    // Renamed in a tenant, the name is that tenant's (a fact "name" kept
    // there, #110), and the others call it as they did; in the graph of no
    // tenant, the whole library's or a world's, it is the thing's own name.
    bool edit(qint64 node, const QString &name, const QString &summary);

    std::optional<GraphNode> node(qint64 id) const;
    // Where a node was met: its earliest place in each book it is in.
    QList<GraphPlace> seen(qint64 node) const;
    // The same in one book, with the words there that name it.
    std::optional<GraphMoment> firstMet(qint64 node, qint64 bookId) const;

    // What was learned of a node over time, in the order of its books and
    // places, the facts of no book last.
    QList<GraphFact> facts(qint64 node) const;
    // The facts of many nodes at once, by node.
    QHash<qint64, QList<GraphFact>> factsOf(const QList<qint64> &nodes) const;

    // Where a thing of the general graph is tied to a book: met in it (with
    // the place and the words there), asked of it (a question with no place,
    // `at` of no book), or related to the book's own node (at the place the
    // relation was learned, of no book when it was made by hand).
    struct Sighting {
        GraphNode node;
        qint64 bookId = 0;
        GraphMoment at;
    };
    // Every such tie of the things of these kinds (of whose data only the
    // other names are read), and the facts of those things by node: what a
    // search over what is known reads, in a few statements however large
    // the graph; with `only`, of those things alone.
    QList<Sighting> sightings(const QStringList &kinds, const std::optional<QList<qint64>> &only = std::nullopt) const;
    QHash<qint64, QList<GraphFact>> factsOfKinds(const QStringList &kinds, const std::optional<QList<qint64>> &only = std::nullopt) const;
    // The things with a place in these books: met there, or a fact or a
    // relation learned there. What a reader who read on there may know more of.
    [[nodiscard]] QSet<qint64> placedIn(const QList<qint64> &books) const;
    // A node as it is known to whoever `known` says knows a place: its
    // summary the latest it had by then, and only the names it was known by
    // by then. The facts of `bookId` come first, by place; a fact of no book
    // put in after them, or failing them what other books said, stands for
    // the summary otherwise. A node of which no fact was kept is as it is.
    static GraphNode asKnown(GraphNode node, const QList<GraphFact> &facts, qint64 bookId, const GraphKnown &known);
    // A node as a reader of a book at a place knows it (see ofBook).
    GraphNode asOf(const GraphNode &node, qint64 bookId, const std::optional<GraphPlace> &upTo) const;
    // A node's timeline: where it was met in each book, each summary it had
    // and each name it was given, and the relations it was given, each with
    // its moment, in the order a reader comes to them. With a book, only
    // what that book revealed and what no book did.
    QList<GraphReveal> timeline(qint64 node, qint64 bookId = 0) const;
    // The node known by a name or alias, or by a name a tenant gave it by
    // hand (#110); 0 when there is none.
    qint64 named(const QString &scope, const QString &kind, const QString &name) const;
    // Nodes whose name, aliases or summary hold every word, each as the start
    // of one of their words, case and accents aside (through the graph's
    // word index, GraphIndex), by name; `kind` and `scope` narrow it when
    // given (scope: nothing for every scope). A word of no letter or digit
    // narrows nothing.
    QList<GraphNode> find(const QString &words, const QString &kind = {}, const std::optional<QString> &scope = QString(),
                          int limit = 50) const;

    // A node and what is within `depth` edges of it.
    GraphView around(qint64 node, int depth = 1, int limit = 200) const;
    // What a book's reader has met: the nodes seen in it and the edges learned
    // in it, with `upTo` only those at or before that place, and each node as
    // it was known there (asOf): never a summary or a name from later. It is the
    // general graph's unless a scope is named: a world copied from what the
    // books say keeps where its things were met, and is not the book's graph.
    GraphView ofBook(qint64 bookId, const std::optional<GraphPlace> &upTo = std::nullopt, const QString &scope = {}) const;
    // The things met in a book, in the order its reader first met them, each
    // with that place. What a reader at some place has met is therefore the
    // front of this list, up to the first thing met after it.
    QList<std::pair<GraphNode, GraphPlace>> metIn(qint64 bookId, const QString &scope = {}) const;
    // Everything that reads (node, seen, facts, the views and search) keeps
    // to the graph's tenant, when it has one (ADR 0003): nothing another
    // tenant sees alone is answered, and a thing both see is told as this
    // one knows it, by its facts here (told()). So a connection across books
    // of two tenants is never answered, and beyond leaves out the books of
    // another tenant.

    // What a book connects to elsewhere: the things of other books that a
    // connection across books (see GraphEdge::across) ties to this book's
    // things, and the other books that a thing of this book is also in (a
    // theme two books share), as those books' own nodes. The edges are the
    // ties themselves: each has one end among ofBook(bookId, upTo).nodes and
    // the other among the nodes answered here. With `upTo`, only this
    // book's things met up to that place, and only the connections noticed
    // in this book up to it.
    GraphView beyond(qint64 bookId, const std::optional<GraphPlace> &upTo = std::nullopt) const;
    // The connections across books there are, those between the most
    // connected things first (by their degree in the tenant), with the
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

    // The reading log: that a book went from one percent to another today,
    // in at least `seconds` (what another computer says of the same day).
    void logReading(qint64 bookId, double from, double to, const QString &day = {}, qint64 seconds = 0);
    QList<ReadingDay> readingLog(qint64 bookId = 0) const;

private:
    // Keeps a fact as learn() does, without touching the node itself.
    qint64 recordFact(qint64 node, const QString &kind, const QString &value, const std::optional<GraphMoment> &revealed);
    // The nodes that meet a condition, of the tenant, in an order, at most `limit`.
    [[nodiscard]] GraphView collect(const QString &nodesWhere, const QVariantList &values, int limit, const QString &order) const;
    [[nodiscard]] QList<GraphEdge> edgesAmong(const QList<GraphNode> &nodes) const;
    // `data` of a thing put in (upsert), kept in the graph's tenant
    // (Tenancy::madeInKey) when it was put in there by hand.
    [[nodiscard]] QJsonObject madeIn(QJsonObject data, qint64 id, const QString &scope, const std::optional<GraphPlace> &seen) const;
    // The tenant a fact or a relation of no book at a thing is written in by
    // hand (its global id), "" when it is not one tenant's: the graph of no
    // tenant's, the whole library's, or a world's (#109).
    [[nodiscard]] QString writtenIn(qint64 node) const;
    // edit() of a thing renamed in the graph's tenant, as it is shown there
    // (a name of the tenant's own, #110), and of its own name otherwise.
    bool editHere(const GraphNode &shown, const QString &called, const QString &said);
    bool editOwn(qint64 id, const QString &called, const QString &said);
    // Relations as the tenant knows them: each with the note it was told
    // there, by hand or by one of its books, never another tenant's (#109).
    void told(QList<GraphEdge> &edges) const;
    // A relation of one thing moved to others (merge), with all it was noted.
    void relink(const GraphEdge &edge, qint64 source, qint64 target);
    // " AND " the tenant's condition on a column (TenantFilter), or nothing
    // for the graph of no tenant.
    [[nodiscard]] QString andWithin(QString (TenantFilter::*condition)(QStringView) const, QStringView column) const;
    // Things as the tenant knows them: of what is said of each and what it is
    // called, only what the tenant was told (its facts). A thing another
    // tenant was told nothing of is as it is.
    void told(QList<GraphNode> &nodes) const;
    // Of a page of things the word index found for these phrases (find()),
    // those that hold the words as the tenant was told of them.
    [[nodiscard]] QList<GraphNode> holding(QList<GraphNode> page, const QStringList &phrases, const QStringList &words) const;

    const Library *m_library;
    std::optional<Tenant> m_tenant;
    mutable std::optional<TenantFilter> m_within;
    mutable std::optional<QSet<qint64>> m_books;
};
