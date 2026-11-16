#include "graphsync.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <limits>
#include <optional>

#include "graph.h"

namespace {

// The kinds a library makes of its own books, on each side (Graph::bookNode).
bool madeHere(const QString &kind) { return kind == "book" || kind == "author" || kind == "series" || kind == "tag"; }

QJsonObject placeJson(const GraphPlace &place, const QString &anchor = {}) {
    QJsonObject object{{"book", place.bookId}, {"section", place.section}, {"fraction", place.fraction}};
    if (!anchor.isEmpty())
        object.insert("anchor", anchor);
    return object;
}

// A book as both sides of a sync know it: its files, or what it is called.
QJsonObject bookName(const Library &library, qint64 id) {
    QJsonArray hashes;
    for (const FormatFile &file : library.formatFiles(id))
        hashes << file.hash;
    if (!hashes.isEmpty())
        return {{"hashes", hashes}};
    const std::optional<Book> book = library.book(id);
    return {{"keys", QJsonArray::fromStringList(book ? Library::entryKeys(*book) : QStringList())}};
}

// --- What goes ----------------------------------------------------------

QJsonObject nodeJson(const Graph &graph, const GraphNode &node, const QList<GraphFact> &facts, QSet<qint64> &books) {
    QJsonObject data = node.data;
    data.remove("aliases");
    QJsonArray seen;
    QJsonArray learned;
    for (const GraphPlace &place : graph.seen(node.id)) {
        seen << placeJson(place, graph.firstMet(node.id, place.bookId).value_or(GraphMoment{}).anchor);
        books << place.bookId;
    }
    for (const GraphFact &fact : facts) {
        QJsonObject object = placeJson(fact.revealed.place, fact.revealed.anchor);
        object.insert("kind", fact.kind);
        object.insert("value", fact.value);
        learned << object;
        books << fact.revealed.place.bookId;
    }
    return {{"id", node.id},
            {"kind", node.kind},
            {"name", node.name},
            {"summary", node.summary},
            {"updated", node.updated},
            {"data", data},
            {"aliases", QJsonArray::fromStringList(node.aliases())},
            {"seen", seen},
            {"facts", learned}};
}

QJsonArray edgesJson(const QList<GraphEdge> &edges, QSet<qint64> &books) {
    QJsonArray array;
    for (const GraphEdge &edge : edges) {
        QJsonObject object = placeJson(edge.learned, edge.anchor);
        object.insert("source", edge.source);
        object.insert("target", edge.target);
        object.insert("kind", edge.kind);
        object.insert("note", edge.note);
        array << object;
        books << edge.learned.bookId;
    }
    return array;
}

} // namespace

QJsonObject GraphSync::bundle(const Library &library) {
    const Graph graph(library);
    const GraphView view = graph.whole({}, std::numeric_limits<int>::max());
    QList<qint64> ids;
    QHash<qint64, GraphNode> made; // the things made from the library, which each side makes itself
    for (const GraphNode &node : view.nodes) {
        ids << node.id;
        if (madeHere(node.kind))
            made.insert(node.id, node);
    }
    const QHash<qint64, QList<GraphFact>> facts = graph.factsOf(ids);
    QSet<qint64> books;
    QJsonArray nodes;
    for (const GraphNode &node : view.nodes)
        if (!made.contains(node.id))
            nodes << nodeJson(graph, node, facts.value(node.id), books);
    // Relations between two made things are made on each side too; the
    // others travel, with the made things at their ends named for what they stand for.
    QList<GraphEdge> edges;
    QJsonObject ends;
    for (const GraphEdge &edge : view.edges) {
        if (made.contains(edge.source) && made.contains(edge.target))
            continue;
        edges << edge;
        for (const qint64 id : {edge.source, edge.target})
            if (const auto end = made.constFind(id); end != made.cend()) {
                ends.insert(QString::number(id), QJsonObject{{"kind", end->kind}, {"name", end->name}, {"book", end->bookId}});
                books << end->bookId;
            }
    }
    QJsonArray log;
    for (const ReadingDay &day : graph.readingLog()) {
        log << QJsonObject{{"book", day.bookId}, {"day", day.day}, {"from", day.from}, {"to", day.to}};
        books << day.bookId;
    }
    const QJsonArray relations = edgesJson(edges, books);
    QJsonObject named;
    for (const qint64 book : std::as_const(books))
        if (book > 0)
            named.insert(QString::number(book), bookName(library, book));
    return {{"version", 1}, {"books", named}, {"nodes", nodes}, {"ends", ends}, {"edges", relations}, {"log", log}};
}

// --- What comes ---------------------------------------------------------

namespace {

// One bundle being taken in: their numbers, of books and things, as this
// library's, and how much was new.
struct Taking {
    Graph graph;
    QHash<qint64, qint64> books; // their book: this library's
    QHash<qint64, qint64> nodes; // their thing: this library's
    int changed = 0;

    explicit Taking(const Library &into) : graph(into) {}

    // Their book here: 0 for no book, nothing for a book this library doesn't have.
    std::optional<qint64> book(qint64 theirs) const {
        if (theirs == 0)
            return 0;
        const qint64 mine = books.value(theirs);
        return mine > 0 ? std::optional(mine) : std::nullopt;
    }
    std::optional<GraphPlace> place(const QJsonObject &object) const {
        const std::optional<qint64> mine = book(object.value("book").toInteger());
        if (!mine)
            return std::nullopt;
        return GraphPlace{*mine, object.value("section").toInt(), object.value("fraction").toDouble()};
    }
    std::optional<GraphMoment> moment(const QJsonObject &object) const {
        const std::optional<GraphPlace> at = place(object);
        return at ? std::optional(GraphMoment{*at, object.value("anchor").toString()}) : std::nullopt;
    }
};

void nameBooks(Taking &taking, const Library &library, const QJsonObject &named) {
    QHash<QString, qint64> byKey;
    for (const Book &book : library.books())
        for (const QString &key : Library::entryKeys(book))
            byKey.insert(key, book.id);
    for (auto entry = named.begin(); entry != named.end(); ++entry) {
        const QJsonObject book = entry.value().toObject();
        qint64 mine = 0;
        for (const auto &hash : book.value("hashes").toArray())
            mine = mine ? mine : library.bookWithHash(hash.toString());
        for (const auto &key : book.value("keys").toArray())
            mine = mine ? mine : byKey.value(key.toString());
        if (mine > 0)
            taking.books.insert(entry.key().toLongLong(), mine);
    }
}

// The thing here that is theirs: by its name, or by one of its other names.
qint64 known(const Taking &taking, const QJsonObject &node) {
    const QString kind = node.value("kind").toString();
    if (const qint64 id = taking.graph.named({}, kind, node.value("name").toString()))
        return id;
    for (const auto &alias : node.value("aliases").toArray())
        if (const qint64 id = taking.graph.named({}, kind, alias.toString()))
            return id;
    return 0;
}

// What it holds that this one doesn't (never its other names: those come
// with the moments they were revealed).
QJsonObject missingData(const GraphNode &mine, const QJsonObject &theirs) {
    QJsonObject missing;
    for (auto value = theirs.begin(); value != theirs.end(); ++value)
        if (value.key() != "aliases" && !mine.data.contains(value.key()))
            missing.insert(value.key(), value.value());
    return missing;
}

void takeSeen(Taking &taking, qint64 id, const GraphNode &node, const QJsonArray &seen) {
    for (const auto &value : seen) {
        const std::optional<GraphPlace> place = taking.place(value.toObject());
        if (!place || place->bookId == 0)
            continue;
        const std::optional<GraphMoment> mine = taking.graph.firstMet(id, place->bookId);
        if (mine && mine->place.notAfter(*place))
            continue;
        taking.graph.upsert({}, node.kind, node.name, {}, {}, place, value.toObject().value("anchor").toString());
        ++taking.changed;
    }
}

bool alreadyKnown(const QList<GraphFact> &mine, const QString &kind, const QString &value, const std::optional<GraphMoment> &moment) {
    const qint64 book = moment ? moment->place.bookId : 0;
    return std::ranges::any_of(mine, [&](const GraphFact &fact) {
        return fact.kind == kind && fact.value == value && fact.revealed.place.bookId == book
               && (book == 0 || fact.revealed.place.notAfter(moment->place));
    });
}

void takeFacts(Taking &taking, qint64 id, const QJsonObject &node) {
    const QList<GraphFact> mine = taking.graph.facts(id);
    QStringList placedNames;
    for (const auto &value : node.value("facts").toArray()) {
        const QJsonObject fact = value.toObject();
        const QString kind = fact.value("kind").toString();
        const QString said = fact.value("value").toString();
        if (kind == "alias")
            placedNames << said;
        const std::optional<GraphMoment> moment = taking.moment(fact);
        if (fact.value("book").toInteger() != 0 && !moment)
            continue; // of a book this library doesn't have
        const std::optional<GraphMoment> at = moment && moment->place.bookId > 0 ? moment : std::nullopt;
        if (!alreadyKnown(mine, kind, said, at) && taking.graph.learn(id, kind, said, at))
            ++taking.changed;
    }
    // Other names with no moment (from before moments were kept) go as they were.
    const std::optional<GraphNode> now = taking.graph.node(id);
    if (!now)
        return;
    for (const auto &alias : node.value("aliases").toArray())
        if (!placedNames.contains(alias.toString()) && !now->aliases().contains(alias.toString(), Qt::CaseInsensitive)
            && taking.graph.learn(id, "alias", alias.toString()))
            ++taking.changed;
}

// What is said of it now: theirs when it changed there last, or when this
// one says nothing; each with the moment it was revealed, never a new one.
void settleSummary(Taking &taking, qint64 id, const GraphNode &before, const QJsonObject &theirs) {
    const QString said = theirs.value("summary").toString();
    const bool takeTheirs = !said.isEmpty() && (before.summary.isEmpty() || theirs.value("updated").toInteger() > before.updated);
    const QString wanted = takeTheirs ? said : before.summary;
    const std::optional<GraphNode> now = taking.graph.node(id);
    if (wanted.isEmpty() || !now || now->summary == wanted)
        return;
    for (const GraphFact &fact : taking.graph.facts(id))
        if (fact.kind == "summary" && fact.value == wanted) {
            taking.graph.learn(id, "summary", wanted, fact.revealed.place.bookId > 0 ? std::optional(fact.revealed) : std::nullopt);
            return;
        }
}

void takeNode(Taking &taking, const QJsonObject &theirs) {
    const QString kind = theirs.value("kind").toString();
    const QJsonObject data = theirs.value("data").toObject();
    const qint64 found = known(taking, theirs);
    const std::optional<GraphNode> before = found ? taking.graph.node(found) : std::nullopt;
    const qint64 id = before ? before->id : taking.graph.upsert({}, kind, theirs.value("name").toString(), {}, missingData({}, data));
    const std::optional<GraphNode> node = taking.graph.node(id);
    if (!node)
        return;
    if (!before) {
        ++taking.changed;
    } else if (const QJsonObject missing = missingData(*before, data); !missing.isEmpty()) {
        taking.graph.upsert({}, kind, before->name, {}, missing);
        ++taking.changed;
    }
    taking.nodes.insert(theirs.value("id").toInteger(), id);
    takeSeen(taking, id, *node, theirs.value("seen").toArray());
    takeFacts(taking, id, theirs);
    settleSummary(taking, id, before.value_or(GraphNode{}), theirs);
}

// Their end of a relation here: a thing taken in, or one this library makes itself.
qint64 end(Taking &taking, const QJsonObject &ends, qint64 theirs) {
    if (const qint64 mine = taking.nodes.value(theirs))
        return mine;
    const QJsonObject made = ends.value(QString::number(theirs)).toObject();
    if (made.value("kind").toString() == "book") {
        const std::optional<qint64> book = taking.book(made.value("book").toInteger());
        return book && *book > 0 ? taking.graph.bookNode(*book) : 0;
    }
    return made.isEmpty() ? 0 : taking.graph.named({}, made.value("kind").toString(), made.value("name").toString());
}

// A relation by its ends and kind, as Graph::link tells one from another.
QString edgeKey(qint64 source, qint64 target, const QString &kind) { return QString("%1 %2 %3").arg(source).arg(target).arg(kind); }

QHash<QString, GraphEdge> edgesHere(const Graph &graph) {
    QHash<QString, GraphEdge> edges;
    for (const GraphEdge &edge : graph.whole({}, std::numeric_limits<int>::max()).edges)
        edges.insert(edgeKey(edge.source, edge.target, edge.kind), edge);
    return edges;
}

// Whether a relation there says more than the same one here: it is new, it
// was learned earlier in the same book, or it has a note where this has none.
bool saysMore(const std::optional<GraphEdge> &mine, const GraphPlace &learned, const QString &note) {
    if (!mine)
        return true;
    const bool earlier =
        learned.bookId > 0 && (mine->learned.bookId == 0 || (mine->learned.bookId == learned.bookId && !mine->learned.notAfter(learned)));
    return earlier || (mine->note.isEmpty() && !note.isEmpty());
}

void takeEdges(Taking &taking, const QJsonArray &edges, const QJsonObject &ends) {
    const QHash<QString, GraphEdge> here = edgesHere(taking.graph);
    for (const auto &value : edges) {
        const QJsonObject edge = value.toObject();
        const qint64 source = end(taking, ends, edge.value("source").toInteger());
        const qint64 target = end(taking, ends, edge.value("target").toInteger());
        const std::optional<GraphPlace> learned = taking.place(edge);
        if (source == 0 || target == 0 || !learned)
            continue;
        const QString kind = edge.value("kind").toString();
        const QString note = edge.value("note").toString();
        const auto mine = here.constFind(edgeKey(source, target, kind.simplified().toLower()));
        if (!saysMore(mine == here.cend() ? std::nullopt : std::optional(*mine), *learned, note))
            continue;
        if (taking.graph.link(source, target, kind, note, learned->bookId > 0 ? learned : std::nullopt, edge.value("anchor").toString()))
            ++taking.changed;
    }
}

void takeLog(Taking &taking, const QJsonArray &log) {
    for (const auto &value : log) {
        const QJsonObject day = value.toObject();
        const std::optional<qint64> book = taking.book(day.value("book").toInteger());
        if (!book || *book == 0)
            continue;
        const QString date = day.value("day").toString();
        const double from = day.value("from").toDouble();
        const double to = day.value("to").toDouble();
        const QList<ReadingDay> mine = taking.graph.readingLog(*book);
        const auto known = std::ranges::find(mine, date, &ReadingDay::day);
        if (known != mine.end() && known->from <= from && known->to >= to)
            continue;
        taking.graph.logReading(*book, from, to, date);
        ++taking.changed;
    }
}

} // namespace

// NOLINTNEXTLINE(misc-const-correctness): it writes the library, through the Graph it makes of it.
int GraphSync::take(Library &library, const QJsonObject &bundle) {
    if (bundle.value("version").toInt() != 1)
        return 0;
    const Library::Batch batch(library);
    Taking taking(library);
    nameBooks(taking, library, bundle.value("books").toObject());
    for (const auto &node : bundle.value("nodes").toArray())
        takeNode(taking, node.toObject());
    takeEdges(taking, bundle.value("edges").toArray(), bundle.value("ends").toObject());
    takeLog(taking, bundle.value("log").toArray());
    return taking.changed;
}
