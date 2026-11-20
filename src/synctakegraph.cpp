#include "synctakegraph.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <limits>
#include <optional>

#include "changelog.h"
#include "graph.h"
#include "graphmatch.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

bool madeHere(const QString &kind) { return kind == "book"_L1 || kind == "author"_L1 || kind == "series"_L1 || kind == "tag"_L1; }

[[nodiscard]] QString fieldOf(const ReducedNode &node, const QString &field) { return node.fields.value(field).value; }

// One take of the graph in progress: this side's things by the id they stand
// for, and how much was new.
struct Taking {
    Library &library;                     // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    const ReducedState &merged;           // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    const QHash<QString, qint64> &books;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    const QHash<QString, QString> &same;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    const QSet<QString> &theirs;          // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    const QHash<QString, QString> &names; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    Graph graph;
    QHash<QString, qint64> nodes;
    QSet<QString> worlds; // the scopes of the worlds things were taken of
    int changed = 0;

    [[nodiscard]] QString standsFor(const QString &gid) const { return same.value(gid, gid); }

    [[nodiscard]] std::optional<GraphPlace> place(const QString &bookGid, const BookPlace &at) const {
        if (bookGid.isEmpty())
            return GraphPlace{};
        const qint64 book = books.value(bookGid);
        return book > 0 ? std::optional(GraphPlace{book, at}) : std::nullopt;
    }
};

// Its other names: in its data, and those learned as facts.
[[nodiscard]] QStringList aliasesOf(const Taking &taking, const QString &gid, const ReducedNode &node) {
    QStringList aliases;
    for (const ReducedFact &fact : taking.merged.facts)
        if (fact.kind == "alias"_L1 && taking.standsFor(fact.nodeGid) == gid && !SpaceReducer::isRemoved(fact.alive, fact.removed))
            aliases << fact.value;
    for (const auto &alias : QJsonDocument::fromJson(fieldOf(node, u"data"_s).toUtf8()).object().value("aliases").toArray())
        aliases << alias.toString();
    for (auto alias = node.aliases.cbegin(); alias != node.aliases.cend(); ++alias)
        if (alias->value == "1"_L1)
            aliases << alias.key();
    return aliases;
}

// The other side's name of a thing this one calls otherwise, kept as one of
// its other names, at the earliest moment met in each book this library
// knows it in (as GraphSync's takeTheirName does, so both sides agree).
// Whether `facts` keep `called` as another name, in the book of `at` no later than it (or at no moment).
[[nodiscard]] bool knownAs(const QList<GraphFact> &facts, const QString &called, const std::optional<GraphMoment> &at) {
    return std::ranges::any_of(facts, [&](const GraphFact &fact) {
        return fact.kind == "alias"_L1 && fact.value == called && fact.revealed.place.bookId == (at ? at->place.bookId : 0)
               && (!at || fact.revealed.place.notAfter(at->place));
    });
}

void learnTheirName(Taking &taking, qint64 id, const QString &called) {
    const std::optional<GraphNode> now = taking.graph.node(id);
    if (!now || called.isEmpty() || now->name.compare(called, Qt::CaseInsensitive) == 0)
        return;
    const QList<GraphFact> facts = taking.graph.facts(id);
    const QList<GraphPlace> seen = taking.graph.seen(id);
    if (seen.isEmpty() && !knownAs(facts, called, std::nullopt) && taking.graph.learn(id, u"alias"_s, called) > 0)
        ++taking.changed;
    for (const GraphPlace &place : seen)
        if (const std::optional<GraphMoment> met = place.bookId > 0 ? taking.graph.firstMet(id, place.bookId) : std::nullopt;
            met && !knownAs(facts, called, met) && taking.graph.learn(id, u"alias"_s, called, met) > 0)
            ++taking.changed;
}

// A thing the other side knows and this one doesn't by that id: `matched`,
// the one here of another name, linked once; or a new one; or none.
qint64 thingFor(Taking &taking, const QString &gid, const ReducedNode &node, qint64 matched) {
    // Unmatched, but going by the name (or another name) of one here, which the
    // other side holds apart from it: a duplicate there, of which nothing is
    // taken, so neither side's thing becomes the other's.
    if (matched == 0 && taking.graph.named({}, fieldOf(node, u"kind"_s), fieldOf(node, u"name"_s)) > 0)
        return 0;
    if (const qint64 same = matched; same > 0) {
        if (const QString mine = taking.standsFor(Spaces::nodeId(taking.library, same)); mine != gid)
            ChangeLog::append(taking.library, {.space = Spaces::personalSpace(taking.library),
                                               .entity = u"node"_s,
                                               .id = std::max(mine, gid),
                                               .field = u"same_as"_s,
                                               .value = std::min(mine, gid),
                                               .hlc = std::nullopt});
        return same;
    }
    ++taking.changed;
    QJsonObject data = QJsonDocument::fromJson(fieldOf(node, u"data"_s).toUtf8()).object();
    data.remove("aliases");
    return taking.graph.upsert({}, fieldOf(node, u"kind"_s), fieldOf(node, u"name"_s), {}, data);
}

void takeSeen(Taking &taking, qint64 id, const ReducedNode &node) {
    for (auto seen = node.seen.cbegin(); seen != node.seen.cend(); ++seen) {
        const std::optional<GraphPlace> at = taking.place(seen.key(), seen->place);
        if (!at || at->bookId == 0)
            continue;
        const std::optional<GraphMoment> mine = id > 0 ? taking.graph.firstMet(id, at->bookId) : std::nullopt;
        if (mine && mine->place.notAfter(*at))
            continue;
        // By the name it has here: the log's may be the other side's.
        if (const std::optional<GraphNode> here = taking.graph.node(id)) {
            ++taking.changed;
            taking.graph.upsert(here->scope, here->kind, here->name, {}, {}, at, seen->anchor);
        }
    }
}

// The things of the general graph that aren't made of books, where they were met.
[[nodiscard]] bool isThing(const ReducedNode &node) {
    const QString kind = fieldOf(node, u"kind"_s);
    return !SpaceReducer::isRemoved(node.alive, node.removed) && !madeHere(kind) && fieldOf(node, u"scope"_s).isEmpty() && !kind.isEmpty()
           && !fieldOf(node, u"name"_s).isEmpty();
}

// The things of the general graph that aren't made of books, where they were
// met. Those this side doesn't know by their id are matched all at once by
// the graph sync's rules (never two of them to one thing here), and linked.
void takeThings(Taking &taking) {
    for (const GraphNode &local : taking.graph.whole({}, std::numeric_limits<int>::max()).nodes)
        taking.nodes.insert(taking.standsFor(Spaces::nodeId(taking.library, local.id)), local.id);
    QStringList gids;
    // The other side's things, matched together as a bundle of theirs is, so
    // one it knows by this side's id takes that thing first; and any other
    // this side doesn't know.
    QJsonArray named;
    for (auto node = taking.merged.nodes.cbegin(); node != taking.merged.nodes.cend(); ++node)
        if (isThing(*node) && (taking.theirs.contains(node.key()) || !taking.nodes.contains(node.key()))) {
            named << QJsonObject{{"id", gids.size()},
                                 {"kind", fieldOf(*node, u"kind"_s)},
                                 {"name", fieldOf(*node, u"name"_s)},
                                 {"aliases", QJsonArray::fromStringList(aliasesOf(taking, node.key(), *node))}};
            gids << node.key();
        }
    const QList<qint64> matched = GraphMatch::matchNodes(taking.graph, named);
    for (qsizetype i = 0; i < gids.size(); ++i)
        if (!taking.nodes.contains(gids.at(i)))
            taking.nodes.insert(gids.at(i), thingFor(taking, gids.at(i), taking.merged.nodes[gids.at(i)], matched.at(i)));
    for (auto node = taking.merged.nodes.cbegin(); node != taking.merged.nodes.cend(); ++node)
        if (isThing(*node))
            takeSeen(taking, taking.nodes.value(node.key()), *node);
}

// The things of a story's world (a scope of its own, `world:…`): the
// world's alone, never matched with the general graph's, and known by the
// same id on every computer, which is made of the scope, kind and name.
void takeWorldThings(Taking &taking) {
    for (auto node = taking.merged.nodes.cbegin(); node != taking.merged.nodes.cend(); ++node) {
        const QString scope = fieldOf(*node, u"scope"_s);
        const QString kind = fieldOf(*node, u"kind"_s);
        const QString name = fieldOf(*node, u"name"_s);
        if (SpaceReducer::isRemoved(node->alive, node->removed) || scope.isEmpty() || kind.isEmpty() || name.isEmpty() || madeHere(kind))
            continue;
        qint64 id = taking.graph.named(scope, kind, name);
        if (id == 0) {
            QJsonObject data = QJsonDocument::fromJson(fieldOf(*node, u"data"_s).toUtf8()).object();
            data.remove("aliases");
            id = taking.graph.upsert(scope, kind, name, {}, data);
            ++taking.changed;
        }
        taking.nodes.insert(taking.standsFor(node.key()), id);
        taking.worlds.insert(scope);
        takeSeen(taking, id, *node);
    }
}

// Every name a linked thing goes by on either side, it goes by here: last, once
// all it was met in is known.
void takeNames(Taking &taking) {
    for (auto name = taking.names.cbegin(); name != taking.names.cend(); ++name)
        if (const qint64 id = taking.nodes.value(taking.standsFor(name.key())))
            learnTheirName(taking, id, name.value());
}

[[nodiscard]] bool alreadyKnown(const QList<GraphFact> &mine, const ReducedFact &fact, const std::optional<GraphMoment> &at) {
    const qint64 book = at ? at->place.bookId : 0;
    return std::ranges::any_of(mine, [&](const GraphFact &known) {
        return known.kind == fact.kind && known.value == fact.value && known.revealed.place.bookId == book
               && (book == 0 || known.revealed.place.notAfter(at->place));
    });
}

void takeFacts(Taking &taking) {
    for (auto fact = taking.merged.facts.cbegin(); fact != taking.merged.facts.cend(); ++fact) {
        const QString gid = taking.standsFor(fact->nodeGid);
        const qint64 node = taking.nodes.value(gid);
        // What is known of the things made of books is made here too.
        const bool made = madeHere(fieldOf(taking.merged.nodes.value(gid), u"kind"_s));
        if (SpaceReducer::isRemoved(fact->alive, fact->removed) || node == 0 || made || fact->kind.isEmpty() || fact->value.isEmpty())
            continue;
        const std::optional<GraphPlace> at = taking.place(fact->bookGid, fact->moment.place);
        if (!at)
            continue; // of a book this library doesn't have
        const std::optional<GraphMoment> moment = at->bookId > 0 ? std::optional(GraphMoment{*at, fact->moment.anchor}) : std::nullopt;
        // A name it goes by already is kept as no fact: nothing is new then.
        if (!alreadyKnown(taking.graph.facts(node), *fact, moment) && taking.graph.learn(node, fact->kind, fact->value, moment) > 0)
            ++taking.changed;
    }
}

[[nodiscard]] QString kindOf(const Taking &taking, const QString &gid) {
    return fieldOf(taking.merged.nodes.value(taking.standsFor(gid)), u"kind"_s);
}

// An end of a relation here: a thing taken in, or one this library makes of its books.
[[nodiscard]] qint64 endOf(Taking &taking, const QString &end) {
    const QString gid = taking.standsFor(end);
    if (const qint64 known = taking.nodes.value(gid))
        return known;
    const auto made = taking.merged.nodes.constFind(gid);
    if (made == taking.merged.nodes.cend() || !madeHere(fieldOf(*made, u"kind"_s)))
        return 0;
    if (fieldOf(*made, u"kind"_s) == "book"_L1) {
        const qint64 book = taking.books.value(fieldOf(*made, u"book"_s));
        return book > 0 ? taking.graph.bookNode(book) : 0;
    }
    return taking.graph.named({}, fieldOf(*made, u"kind"_s), fieldOf(*made, u"name"_s));
}

// Whether a relation the log knows says more than the same one here: it is
// new, it was learned earlier in the same book, or it has a note where this has none.
[[nodiscard]] bool saysMore(const GraphEdge *mine, const GraphPlace &learned, const QString &note) {
    if (!mine)
        return true;
    const bool earlier =
        learned.bookId > 0 && (mine->learned.bookId == 0 || (mine->learned.bookId == learned.bookId && !mine->learned.notAfter(learned)));
    return earlier || (mine->note.isEmpty() && !note.isEmpty());
}

// Relations between two things made of books are made on each side too.
[[nodiscard]] bool travels(const Taking &taking, const ReducedEdge &edge) {
    return !SpaceReducer::isRemoved(edge.alive, edge.removed) && !edge.kind.isEmpty()
           && !(madeHere(kindOf(taking, edge.sourceGid)) && madeHere(kindOf(taking, edge.targetGid)));
}

void takeEdges(Taking &taking) {
    QHash<QString, GraphEdge> here;
    for (const QString &scope : QStringList{QString()} + taking.worlds.values())
        for (const GraphEdge &edge : taking.graph.whole(scope, std::numeric_limits<int>::max()).edges)
            here.insert(QString("%1 %2 %3").arg(edge.source).arg(edge.target).arg(edge.kind), edge);
    for (const ReducedEdge &edge : taking.merged.edges) {
        if (!travels(taking, edge))
            continue;
        const qint64 source = endOf(taking, edge.sourceGid);
        const qint64 target = endOf(taking, edge.targetGid);
        const std::optional<GraphPlace> learned = taking.place(edge.bookGid, edge.learned.place);
        if (source == 0 || target == 0 || source == target || !learned)
            continue;
        const auto mine = here.constFind(QString("%1 %2 %3").arg(source).arg(target).arg(edge.kind.simplified().toLower()));
        if (!saysMore(mine == here.cend() ? nullptr : &*mine, *learned, edge.note.value))
            continue;
        ++taking.changed;
        taking.graph.link(source, target, edge.kind, edge.note.value, learned->bookId > 0 ? learned : std::nullopt, edge.learned.anchor);
    }
}

void takeDays(Taking &taking) {
    for (const ReducedDay &day : taking.merged.days) {
        const qint64 book = taking.books.value(day.bookGid);
        if (!day.valid || book == 0)
            continue;
        const QList<ReadingDay> mine = taking.graph.readingLog(book);
        const auto known = std::ranges::find(mine, day.day, &ReadingDay::day);
        if (known != mine.end() && known->from <= day.from && known->to >= day.to)
            continue;
        ++taking.changed;
        taking.graph.logReading(book, day.from, day.to, day.day);
    }
}

} // namespace

// NOLINTNEXTLINE(misc-const-correctness): it writes the library, through the Graph it makes of it.
int SyncTakeGraph::take(Library &library, const ReducedState &merged, const QHash<QString, qint64> &books,
                        const QHash<QString, QString> &same, const Things &things) {
    const Library::Batch batch(library);
    Taking taking{.library = library,
                  .merged = merged,
                  .books = books,
                  .same = same,
                  .theirs = things.theirs,
                  .names = things.names,
                  .graph = Graph(library),
                  .nodes = {},
                  .worlds = {},
                  .changed = 0};
    takeThings(taking);
    takeWorldThings(taking);
    takeFacts(taking);
    takeEdges(taking);
    takeDays(taking);
    takeNames(taking);
    return taking.changed;
}
