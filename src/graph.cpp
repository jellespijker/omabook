#include "graph.h"

#include "booksides.h"
#include "graphrows.h"
#include "spaces.h"

#include <QCryptographicHash>
#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <functional>
#include <iterator>
#include <ranges>
#include <tuple>

using namespace Qt::StringLiterals;
using namespace GraphRows;

namespace {

[[nodiscard]] QString compact(const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

// A book's place as a column takes it: no book is NULL, so a fact of no book
// is not tied to one, and a book's facts go with it when it is removed.
[[nodiscard]] QVariant bookColumn(qint64 bookId) { return bookId > 0 ? QVariant(bookId) : QVariant(QMetaType(QMetaType::LongLong)); }

// Later in the same book: a place further on, or the same place kept later.
[[nodiscard]] bool laterThan(const GraphFact &a, const GraphFact &b) {
    const GraphPlace &x = a.revealed.place, &y = b.revealed.place;
    return std::tuple{x.section, x.fraction, a.id} > std::tuple{y.section, y.fraction, b.id};
}

// Where a fact was learned, when a book taught it.
[[nodiscard]] std::optional<GraphPlace> learnedIn(const GraphEdge &edge) {
    return edge.learned.bookId > 0 ? std::optional(edge.learned) : std::nullopt;
}

[[nodiscard]] QString xml(const QString &text) { return text.toHtmlEscaped(); }
// A string literal for Cypher and DOT, which escape alike.
[[nodiscard]] QString quoted(QString text) {
    return "\"" + text.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n") + "\"";
}

} // namespace

QStringList GraphNode::aliases() const {
    QStringList names;
    for (const QJsonValue &alias : data.value("aliases").toArray())
        names << alias.toString();
    return names;
}

QJsonObject GraphNode::toJson() const {
    QJsonObject object{{"id", id}, {"kind", kind}, {"name", name}};
    if (!scope.isEmpty()) object.insert("scope", scope);
    if (!summary.isEmpty()) object.insert("summary", summary);
    if (!data.isEmpty()) object.insert("data", data);
    if (bookId > 0) object.insert("book", bookId);
    return object;
}

namespace {
QJsonObject momentJson(const GraphMoment &moment) {
    QJsonObject object{{"book", moment.place.bookId}, {"section", moment.place.section}, {"fraction", moment.place.fraction}};
    if (!moment.anchor.isEmpty())
        object.insert("anchor", moment.anchor);
    return object;
}
} // namespace

QJsonObject GraphFact::toJson() const {
    QJsonObject object{{"id", id}, {"node", node}, {"kind", kind}, {"value", value}};
    if (revealed.place.bookId > 0)
        object.insert("revealed", momentJson(revealed));
    return object;
}

QJsonObject GraphReveal::toJson() const {
    QJsonObject object{{"what", what}, {"text", text}};
    if (revealed.place.bookId > 0)
        object.insert("revealed", momentJson(revealed));
    if (what == "relation") {
        object.insert("edge", edge);
        object.insert("other", other);
        object.insert("other_name", otherName);
        object.insert("outgoing", outgoing);
    }
    return object;
}

QJsonObject GraphEdge::toJson() const {
    QJsonObject object{{"id", id}, {"source", source}, {"target", target}, {"kind", kind}};
    if (!note.isEmpty()) object.insert("note", note);
    if (learned.bookId > 0)
        object.insert("learned", momentJson({learned, anchor}));
    if (across())
        object.insert("across", true);
    return object;
}

QJsonObject GraphView::toJson() const {
    QJsonArray nodeArray, edgeArray;
    for (const GraphNode &node : nodes)
        nodeArray << node.toJson();
    for (const GraphEdge &edge : edges)
        edgeArray << edge.toJson();
    return {{"nodes", nodeArray}, {"edges", edgeArray}};
}

QByteArray GraphView::exported(const QString &format) const {
    QString out;
    if (format == "json")
        return QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
    if (format == "graphml") {
        out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<graphml xmlns=\"http://graphml.graphdrawing.org/xmlns\">\n"
              "  <key id=\"kind\" for=\"all\" attr.name=\"kind\" attr.type=\"string\"/>\n"
              "  <key id=\"name\" for=\"node\" attr.name=\"name\" attr.type=\"string\"/>\n"
              "  <key id=\"summary\" for=\"node\" attr.name=\"summary\" attr.type=\"string\"/>\n"
              "  <graph edgedefault=\"directed\">\n";
        for (const GraphNode &node : nodes)
            out += QString("    <node id=\"n%1\"><data key=\"kind\">%2</data><data key=\"name\">%3</data>"
                           "<data key=\"summary\">%4</data></node>\n")
                       .arg(node.id).arg(xml(node.kind), xml(node.name), xml(node.summary));
        for (const GraphEdge &edge : edges)
            out += QString("    <edge source=\"n%1\" target=\"n%2\"><data key=\"kind\">%3</data></edge>\n")
                       .arg(edge.source).arg(edge.target).arg(xml(edge.kind));
        out += "  </graph>\n</graphml>\n";
    } else if (format == "dot") {
        out = "digraph omabook {\n";
        for (const GraphNode &node : nodes)
            out += QString("  n%1 [label=%2];\n").arg(node.id).arg(quoted(node.name + "\n(" + node.kind + ")"));
        for (const GraphEdge &edge : edges)
            out += QString("  n%1 -> n%2 [label=%3];\n").arg(edge.source).arg(edge.target).arg(quoted(edge.kind));
        out += "}\n";
    } else if (format == "cypher") {
        // One statement each, so it can be pasted into Neo4j as it is.
        for (const GraphNode &node : nodes)
            out += QString("MERGE (n:Thing {omabook: %1}) SET n.kind = %2, n.name = %3, n.summary = %4;\n")
                       .arg(node.id).arg(quoted(node.kind), quoted(node.name), quoted(node.summary));
        for (const GraphEdge &edge : edges)
            out += QString("MATCH (a:Thing {omabook: %1}), (b:Thing {omabook: %2}) MERGE (a)-[:RELATES {kind: %3}]->(b);\n")
                       .arg(edge.source).arg(edge.target).arg(quoted(edge.kind));
    } else {
        return {};
    }
    return out.toUtf8();
}

Graph::Graph(const Library &library) : m_library(&library) {}

qint64 Graph::named(const QString &scope, const QString &kind, const QString &name) const {
    const QString wanted = name.simplified();
    // The name itself, through the index: the common case, and cheap however
    // large the graph. SQLite folds case only for ASCII, so another spelling
    // of a non-ASCII name is still found by the look through the aliases.
    if (QSqlQuery exact = m_library->query("SELECT id FROM graph_nodes WHERE scope = ? AND kind = ? AND name = ? COLLATE NOCASE LIMIT 1",
                                           {text(scope), kind, wanted});
        exact.next())
        return exact.value(0).toLongLong();
    // Then the nodes whose details mention the name at all, which are few,
    // and of those the one that has it as another name. Where SQLite can't
    // fold the name's case (letters beyond ASCII), every node of the kind is
    // looked at, as before.
    const auto look = [&](const QString &sql, const QVariantList &values) -> qint64 {
        qint64 byAlias = 0;
        for (QSqlQuery query = m_library->query(sql, values); query.next();) {
            if (query.value(1).toString().compare(wanted, Qt::CaseInsensitive) == 0)
                return query.value(0).toLongLong();
            for (const QJsonValue &alias : QJsonDocument::fromJson(query.value(2).toString().toUtf8()).object().value("aliases").toArray())
                if (byAlias == 0 && alias.toString().compare(wanted, Qt::CaseInsensitive) == 0)
                    byAlias = query.value(0).toLongLong();
        }
        return byAlias;
    };
    if (const qint64 found = look("SELECT id, name, data FROM graph_nodes WHERE scope = ? AND kind = ? AND data LIKE ?",
                                  {text(scope), kind, "%" + wanted + "%"}))
        return found;
    const bool ascii = std::ranges::all_of(wanted, [](QChar c) { return c.unicode() < 128; });
    return ascii ? 0 : look("SELECT id, name, data FROM graph_nodes WHERE scope = ? AND kind = ?", {text(scope), kind});
}

qint64 Graph::upsert(const QString &scope, const QString &kind, const QString &name, const QString &summary,
                     const QJsonObject &data, const std::optional<GraphPlace> &seen, const QString &anchor,
                     const std::optional<GraphMoment> &revealed) {
    const QString called = name.simplified();
    if (called.isEmpty() || kind.simplified().isEmpty())
        return 0;
    const qint64 now = Spaces::wallSeconds(*m_library);
    qint64 id = named(scope, kind, called);
    // What it is newly said to be, and newly called, is kept as facts with
    // the moment it was revealed.
    const std::optional<GraphMoment> moment = revealed && revealed->place.bookId > 0 ? revealed
                                              : seen && seen->bookId > 0 ? std::optional(GraphMoment{*seen, anchorText(anchor)})
                                                                         : std::nullopt;
    QStringList newNames;
    const QJsonObject given = madeIn(data, id, scope, seen);
    if (id == 0) {
        const auto added = m_library->insert("INSERT INTO graph_nodes (scope, kind, name, summary, data, updated) VALUES (?, ?, ?, ?, ?, ?)",
                                             {scope, kind.simplified().toLower(), called, summary.trimmed(), compact(given), now});
        if (!added)
            return 0;
        id = *added;
        GraphNode made;
        made.data = data;
        newNames = made.aliases();
    } else {
        const GraphNode known = Graph(*m_library).node(id).value_or(GraphNode());
        QJsonObject merged = known.data;
        QStringList aliases = known.aliases();
        for (auto value = given.begin(); value != given.end(); ++value)
            if (value.key() != "aliases")
                merged.insert(value.key(), value.value());
        for (const QJsonValue &alias : data.value("aliases").toArray())
            if (!aliases.contains(alias.toString(), Qt::CaseInsensitive) && alias.toString().compare(known.name, Qt::CaseInsensitive) != 0)
                aliases << alias.toString();
        // Met under another name: that name is one of its names now.
        if (known.name.compare(called, Qt::CaseInsensitive) != 0 && !aliases.contains(called, Qt::CaseInsensitive))
            aliases << called;
        newNames = aliases.mid(known.aliases().size());
        if (!aliases.isEmpty())
            merged.insert("aliases", QJsonArray::fromStringList(aliases));
        m_library->exec("UPDATE graph_nodes SET summary = ?, data = ?, updated = ? WHERE id = ?",
                        {summary.trimmed().isEmpty() ? known.summary : summary.trimmed(), compact(merged), now, id});
    }
    if (seen && seen->bookId > 0) {
        // The earliest place in each book stands.
        QSqlQuery before = m_library->query("SELECT book_id, section, fraction FROM graph_seen WHERE node_id = ? AND book_id = ?",
                                            {id, seen->bookId});
        if (!before.next() || seen->notAfter(placeFrom(before, 0))) {
            before.finish();
            m_library->exec("INSERT OR REPLACE INTO graph_seen (node_id, book_id, section, fraction, anchor) VALUES (?, ?, ?, ?, ?)",
                            {id, seen->bookId, seen->section, seen->fraction, anchorText(anchor)});
        }
    }
    if (!summary.trimmed().isEmpty())
        recordFact(id, "summary", summary.trimmed(), moment);
    for (const QString &alias : std::as_const(newNames))
        recordFact(id, "alias", alias, moment);
    return id;
}

qint64 Graph::recordFact(qint64 node, const QString &kind, const QString &value, const std::optional<GraphMoment> &revealed) {
    const qint64 bookId = revealed ? revealed->place.bookId : 0;
    const GraphPlace place = revealed && bookId > 0 ? revealed->place : GraphPlace();
    const QString anchor = revealed && bookId > 0 ? anchorText(revealed->anchor) : QString("");
    // Of no book and written by hand in a tenant, it is that tenant's (#109).
    const QString tenant = bookId > 0 ? QString("") : writtenIn(node);
    // Revealed again in the same book: the earlier moment stands. Of no
    // book, the fact is kept anew as the latest word (in its tenant).
    QSqlQuery before = m_library->query(QString("SELECT %1 FROM graph_facts WHERE node_id = ? AND kind = ? AND value = ? "
                                                "AND COALESCE(book_id, 0) = ? AND tenant = ?")
                                            .arg(factColumns),
                                        {node, kind, value, bookId, tenant});
    if (before.next()) {
        const GraphFact known = factFrom(before);
        before.finish();
        if (bookId == 0)
            m_library->exec("DELETE FROM graph_facts WHERE id = ?", {known.id});
        else if (known.revealed.place.notAfter(place))
            return known.id;
        else
            return m_library->exec("UPDATE graph_facts SET section = ?, fraction = ?, anchor = ? WHERE id = ?",
                                   {place.section, place.fraction, anchor, known.id})
                       ? known.id
                       : 0;
    }
    return m_library
        ->insert(
            "INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction, anchor, tenant) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            {node, kind, value, bookColumn(bookId), place.section, place.fraction, anchor, tenant})
        .value_or(0);
}

qint64 Graph::learn(qint64 id, const QString &kind, const QString &value, const std::optional<GraphMoment> &revealed) {
    const QString said = GraphFact::kept(kind, value);
    const auto known = node(id);
    if (!known || said.isEmpty() || (kind != "summary" && kind != "alias" && kind != "when"))
        return 0;
    if (kind == "summary") {
        m_library->exec("UPDATE graph_nodes SET summary = ?, updated = ? WHERE id = ?", {said, Spaces::wallSeconds(*m_library), id});
    } else if (kind == "alias" && known->name.compare(said, Qt::CaseInsensitive) == 0) {
        return 0; // its own name is no other name
    } else if (kind == "alias" && !known->aliases().contains(said, Qt::CaseInsensitive)) {
        QJsonObject data = known->data;
        data.insert("aliases", QJsonArray::fromStringList(known->aliases() << said));
        m_library->exec("UPDATE graph_nodes SET data = ?, updated = ? WHERE id = ?", {compact(data), Spaces::wallSeconds(*m_library), id});
    }
    return recordFact(id, kind, said, revealed);
}

bool Graph::remove(qint64 id) { return node(id) && m_library->exec("DELETE FROM graph_nodes WHERE id = ?", {id}); }
bool Graph::unlink(qint64 edge) {
    // Only a relation the tenant sees is the tenant's to take away.
    return m_library->exec("DELETE FROM graph_edges WHERE id = ?" + andWithin(&TenantFilter::hasEdge, u"graph_edges.id"), {edge});
}

bool Graph::merge(qint64 keep, qint64 other) {
    const Library::Batch batch(*m_library);
    const auto kept = node(keep), folded = node(other);
    if (!kept || !folded || keep == other)
        return false;
    QList<GraphEdge> moving;
    for (QSqlQuery edges = m_library->query(QString("SELECT %1 FROM graph_edges WHERE source = ? OR target = ?").arg(edgeColumns),
                                            {other, other});
         edges.next();)
        moving << edgeFrom(edges);
    for (const GraphEdge &edge : std::as_const(moving))
        relink(edge, edge.source == other ? keep : edge.source, edge.target == other ? keep : edge.target);
    // Its names, each known from where it was: its own name from where it
    // was met, the others from the moments they were revealed, so a name
    // learned late in a book stays out of what an earlier page knows.
    const QList<GraphFact> learned = facts(other);
    const QList<GraphPlace> places = seen(other);
    QList<GraphMoment> met;
    for (const GraphPlace &place : places)
        met << firstMet(other, place.bookId).value_or(GraphMoment{place, {}});
    if (met.isEmpty())
        learn(keep, "alias", folded->name);
    for (const GraphMoment &moment : std::as_const(met))
        learn(keep, "alias", folded->name, moment);
    for (const QString &alias : folded->aliases()) {
        bool placed = false;
        for (const GraphFact &fact : learned)
            if (fact.kind == "alias" && fact.value.compare(alias, Qt::CaseInsensitive) == 0) {
                learn(keep, "alias", alias, fact.revealed.place.bookId > 0 ? std::optional(fact.revealed) : std::nullopt);
                placed = true;
            }
        if (!placed)
            learn(keep, "alias", alias);
    }
    // What was said of it and when it happens, with their moments, go along; the summary of
    // now only where the kept node had none.
    m_library->exec("UPDATE graph_facts SET node_id = ? WHERE node_id = ? AND kind IN ('summary', 'when')", {keep, other});
    if (kept->summary.isEmpty() && !folded->summary.isEmpty())
        m_library->exec("UPDATE graph_nodes SET summary = ? WHERE id = ?", {folded->summary, keep});
    for (const GraphMoment &moment : std::as_const(met))
        upsert(kept->scope, kept->kind, kept->name, {}, {}, moment.place, moment.anchor);
    return remove(other);
}

bool Graph::edit(qint64 id, const QString &name, const QString &summary) {
    const auto known = node(id);
    const QString called = name.simplified();
    if (!known || called.isEmpty())
        return false;
    const qint64 other = named(known->scope, known->kind, called);
    if (other != 0 && other != id)
        return false;
    QJsonObject data = known->data;
    QStringList aliases = known->aliases();
    aliases.removeAll(called);
    if (known->name.compare(called, Qt::CaseInsensitive) != 0 && !aliases.contains(known->name, Qt::CaseInsensitive))
        aliases << known->name;
    if (aliases.isEmpty())
        data.remove("aliases");
    else
        data.insert("aliases", QJsonArray::fromStringList(aliases));
    if (!m_library->exec("UPDATE graph_nodes SET name = ?, summary = ?, data = ?, updated = ? WHERE id = ?",
                         {called, summary.trimmed(), compact(data), Spaces::wallSeconds(*m_library), id}))
        return false;
    // Written by hand, it is the latest word on it, at no place of a book.
    if (summary.trimmed() != known->summary && !summary.trimmed().isEmpty())
        recordFact(id, "summary", summary.trimmed(), std::nullopt);
    // The name it had is one of its names from where it was met, as the
    // name itself was; a name given by hand is known everywhere.
    if (known->name.compare(called, Qt::CaseInsensitive) != 0) {
        m_library->exec("DELETE FROM graph_facts WHERE node_id = ? AND kind = 'alias' AND value = ? COLLATE NOCASE", {id, called});
        const QList<GraphPlace> places = seen(id);
        if (places.isEmpty())
            recordFact(id, "alias", known->name, std::nullopt);
        for (const GraphPlace &place : places)
            recordFact(id, "alias", known->name, GraphMoment{place, firstMet(id, place.bookId).value_or(GraphMoment()).anchor});
    }
    return true;
}

std::optional<GraphNode> Graph::node(qint64 id) const {
    QSqlQuery query =
        m_library->query(QString("SELECT %1 FROM graph_nodes WHERE id = ?%2").arg(nodeColumns, andWithin(&TenantFilter::hasNode, u"graph_nodes.id")), {id});
    if (!query.next())
        return std::nullopt;
    QList<GraphNode> found{nodeFrom(query)};
    query.finish();
    told(found);
    return found.first();
}

QList<GraphPlace> Graph::seen(qint64 id) const {
    QList<GraphPlace> places;
    for (QSqlQuery query = m_library->query(QString("SELECT book_id, section, fraction FROM graph_seen WHERE node_id = ?%1 ORDER BY book_id")
                                                .arg(andWithin(&TenantFilter::hasBook, u"graph_seen.book_id")),
                                            {id});
         query.next();)
        places.append(placeFrom(query, 0));
    return places;
}

std::optional<GraphMoment> Graph::firstMet(qint64 id, qint64 bookId) const {
    if (!sees(bookId))
        return std::nullopt;
    QSqlQuery query = m_library->query("SELECT book_id, section, fraction, anchor FROM graph_seen WHERE node_id = ? AND book_id = ?",
                                       {id, bookId});
    return query.next() ? std::optional(GraphMoment{placeFrom(query, 0), query.value(3).toString()}) : std::nullopt;
}

bool Graph::sees(qint64 bookId) const {
    if (!m_tenant)
        return true;
    if (!m_books)
        m_books = Tenancy::booksOf(*m_library, *m_tenant);
    return m_books->contains(bookId);
}

bool Graph::together(qint64 bookId, qint64 otherBook) const { return BookSides::of(*m_library).together(bookId, otherBook); }

QString Graph::andWithin(QString (TenantFilter::*condition)(QStringView) const, QStringView column) const {
    const std::optional<TenantFilter> &filter = within();
    return filter ? u" AND "_s + std::invoke(condition, *filter, column) : QString();
}

QList<GraphFact> Graph::facts(qint64 id) const { return factsOf({id}).value(id); }

QHash<qint64, QList<GraphFact>> Graph::factsOf(const QList<qint64> &nodes) const {
    QHash<qint64, QList<GraphFact>> facts;
    for (const auto batch : nodes | std::views::chunk(500))
        for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_facts WHERE node_id IN (%2)%4 ORDER BY %3")
                                                    .arg(factColumns, marks(std::ranges::ssize(batch)), factOrder,
                                                         andWithin(&TenantFilter::hasFact, u"graph_facts.id")),
                                                batch | std::ranges::to<QVariantList>());
             query.next();) {
            GraphFact fact = factFrom(query);
            facts[fact.node].append(std::move(fact));
        }
    return facts;
}


GraphNode Graph::asKnown(GraphNode node, const QList<GraphFact> &facts, qint64 bookId, const GraphKnown &known) {
    const GraphFact *ofBook = nullptr, *other = nullptr;
    bool summaries = false;
    QHash<QString, bool> named; // a name by its facts: whether any of them is known
    for (const GraphFact &fact : facts) {
        if (fact.node != node.id)
            continue;
        const bool knownHere = known(fact.revealed.place);
        if (fact.kind == "alias") {
            named[fact.value.toCaseFolded()] |= knownHere;
            continue;
        }
        if (fact.kind != "summary")
            continue;
        summaries = true;
        if (!knownHere)
            continue;
        if (bookId > 0 && fact.revealed.place.bookId == bookId) {
            if (!ofBook || laterThan(fact, *ofBook))
                ofBook = &fact;
        } else if (!other || fact.id > other->id) {
            other = &fact;
        }
    }
    if (summaries) {
        // Written by hand after the book's own word, it is the word on it.
        const bool byHand = other && ofBook && other->revealed.place.bookId == 0 && other->id > ofBook->id;
        const GraphFact *chosen = ofBook && !byHand ? ofBook : other;
        node.summary = chosen ? chosen->value : QString();
    }
    if (!named.isEmpty()) {
        QStringList aliases = node.aliases();
        aliases.removeIf([&named](const QString &alias) {
            const auto found = named.constFind(alias.toCaseFolded());
            return found != named.cend() && !*found;
        });
        if (aliases.isEmpty())
            node.data.remove("aliases");
        else
            node.data.insert("aliases", QJsonArray::fromStringList(aliases));
    }
    return node;
}

GraphNode Graph::asOf(const GraphNode &node, qint64 bookId, const std::optional<GraphPlace> &upTo) const {
    if (!upTo)
        return node;
    return asKnown(node, facts(node.id), bookId, [bookId, place = *upTo](const GraphPlace &at) {
        return at.bookId != bookId || at.notAfter(place);
    });
}


qint64 Graph::bookNode(qint64 bookId) {
    // What the library says of a book is every tenant's that has the book,
    // never made by hand in one.
    if (m_tenant)
        return Graph(*m_library).bookNode(bookId);
    const auto book = m_library->book(bookId);
    if (!book)
        return 0;
    QSqlQuery query = m_library->query("SELECT id, data FROM graph_nodes WHERE kind = 'book' AND book_id = ?", {bookId});
    qint64 id = query.next() ? query.value(0).toLongLong() : 0;
    QJsonObject data{{"status", book->status()}, {"held", book->held()}};
    if (book->rating > 0) data.insert("rating", book->rating);
    if (!book->finished.isEmpty()) data.insert("finished", book->finished);
    if (book->percent > 0) data.insert("percent_read", qRound(book->percent));
    // What the node was last made from. Unchanged, there is nothing to
    // write: bringing a whole library up to date is then a read per book.
    const QString made = QString::fromLatin1(
        QCryptographicHash::hash(QStringList{book->meta.title, book->meta.authors.join('\n'), book->meta.series,
                                             QString::number(book->meta.seriesIndex), book->meta.tags.join('\n'),
                                             compact(data)}
                                     .join('\x1f')
                                     .toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex());
    if (id != 0 && QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object().value("made").toString() == made)
        return id;
    query.finish();
    data.insert("made", made);
    const qint64 now = Spaces::wallSeconds(*m_library);
    if (id == 0) {
        const auto added = m_library->insert(
            "INSERT INTO graph_nodes (scope, kind, name, summary, data, book_id, updated) VALUES ('', 'book', ?, '', ?, ?, ?)",
            {book->meta.title, compact(data), bookId, now});
        if (!added)
            return 0;
        id = *added;
    } else {
        m_library->exec("UPDATE graph_nodes SET name = ?, data = ?, updated = ? WHERE id = ?", {book->meta.title, compact(data), now, id});
    }
    // What the library says of the book, as edges; stale ones are replaced.
    m_library->exec("DELETE FROM graph_edges WHERE source = ? AND kind IN ('wrote', 'part of', 'tagged')", {id});
    m_library->exec("DELETE FROM graph_edges WHERE target = ? AND kind = 'wrote'", {id});
    for (const QString &author : book->meta.authors)
        link(upsert({}, "author", author), id, "wrote");
    if (!book->meta.series.trimmed().isEmpty())
        link(id, upsert({}, "series", book->meta.series), "part of",
             book->meta.seriesIndex > 0 ? QString::number(book->meta.seriesIndex) : QString());
    for (const QString &tag : book->meta.tags)
        link(id, upsert({}, "tag", tag), "tagged");
    return id;
}

void Graph::syncLibrary() {
    if (m_tenant) {
        Graph(*m_library).syncLibrary();
        return;
    }
    const Library::Batch batch(*m_library);
    for (const Book &book : m_library->books())
        bookNode(book.id);
    // Authors, series and tags no book points at any more are left over.
    m_library->exec("DELETE FROM graph_nodes WHERE scope = '' AND kind IN ('author', 'series', 'tag') AND id NOT IN "
                    "(SELECT source FROM graph_edges UNION SELECT target FROM graph_edges)");
}


bool Graph::copyScope(const QString &from, const QString &to) {
    const Library::Batch batch(*m_library);
    if (to.isEmpty() || from == to || scopes().contains(to))
        return false;
    const GraphView source = whole(from, 1000000);
    QHash<qint64, qint64> copies;
    for (const GraphNode &node : source.nodes) {
        const qint64 copy = upsert(to, node.kind, node.name, node.summary, node.data);
        copies.insert(node.id, copy);
        m_library->exec("INSERT OR REPLACE INTO graph_seen (node_id, book_id, section, fraction, anchor) "
                        "SELECT ?, book_id, section, fraction, anchor FROM graph_seen WHERE node_id = ?",
                        {copy, node.id});
        // With what was learned of it and when, so the copy is as careful
        // of a reader's place as the graph it came from.
        m_library->exec("DELETE FROM graph_facts WHERE node_id = ?", {copy});
        m_library->exec("INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction, anchor) "
                        "SELECT ?, kind, value, book_id, section, fraction, anchor FROM graph_facts WHERE node_id = ?",
                        {copy, node.id});
    }
    for (const GraphEdge &edge : source.edges)
        link(copies.value(edge.source), copies.value(edge.target), edge.kind, edge.note, learnedIn(edge), edge.anchor);
    return !source.nodes.isEmpty();
}

bool Graph::removeScope(const QString &scope) {
    // The general graph is not a scope to throw away.
    return !scope.isEmpty() && m_library->exec("DELETE FROM graph_nodes WHERE scope = ?", {scope});
}

void Graph::logReading(qint64 bookId, double from, double to, const QString &day) {
    const QString date = day.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : day;
    QSqlQuery before = m_library->query("SELECT from_percent, to_percent FROM reading_log WHERE book_id = ? AND day = ?", {bookId, date});
    // One row a day: from where the day began to the furthest it got.
    const bool known = before.next();
    m_library->exec("INSERT OR REPLACE INTO reading_log (book_id, day, from_percent, to_percent) VALUES (?, ?, ?, ?)",
                    {bookId, date, known ? std::min(before.value(0).toDouble(), from) : from,
                     known ? std::max(before.value(1).toDouble(), to) : to});
}

QList<ReadingDay> Graph::readingLog(qint64 bookId) const {
    QList<ReadingDay> days;
    for (QSqlQuery query = bookId > 0 ? m_library->query("SELECT book_id, day, from_percent, to_percent FROM reading_log "
                                                         "WHERE book_id = ? ORDER BY day",
                                                         {bookId})
                                      : m_library->query("SELECT book_id, day, from_percent, to_percent FROM reading_log ORDER BY day");
         query.next();)
        days.append({.bookId = query.value(0).toLongLong(),
                     .day = query.value(1).toString(),
                     .from = query.value(2).toDouble(),
                     .to = query.value(3).toDouble()});
    // Of the tenant's books.
    days.removeIf([this](const ReadingDay &day) { return !sees(day.bookId); });
    return days;
}
