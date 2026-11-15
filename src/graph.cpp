#include "graph.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <iterator>
#include <ranges>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString compact(const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

// A null string binds as SQL NULL, which a scope, a summary and a note may not be.
[[nodiscard]] QString text(const QString &value) { return value.isNull() ? QString("") : value; }

[[nodiscard]] GraphNode nodeFrom(const QSqlQuery &query) {
    GraphNode node;
    node.id = query.value(0).toLongLong();
    node.scope = query.value(1).toString();
    node.kind = query.value(2).toString();
    node.name = query.value(3).toString();
    node.summary = query.value(4).toString();
    node.data = QJsonDocument::fromJson(query.value(5).toString().toUtf8()).object();
    node.bookId = query.value(6).toLongLong();
    node.updated = query.value(7).toLongLong();
    return node;
}
constexpr auto nodeColumns = "id, scope, kind, name, summary, data, COALESCE(book_id, 0), updated"_L1;

// A place as three columns of a row hold it, the first of them at `column`.
[[nodiscard]] GraphPlace placeFrom(const QSqlQuery &query, int column) {
    return {.bookId = query.value(column).toLongLong(),
            .section = query.value(column + 1).toInt(),
            .fraction = query.value(column + 2).toDouble()};
}

[[nodiscard]] GraphEdge edgeFrom(const QSqlQuery &query) {
    return {.id = query.value(0).toLongLong(),
            .source = query.value(1).toLongLong(),
            .target = query.value(2).toLongLong(),
            .kind = query.value(3).toString(),
            .note = query.value(4).toString(),
            .learned = placeFrom(query, 5),
            .anchor = query.value(8).toString()};
}
constexpr auto edgeColumns = "id, source, target, kind, note, book_id, section, fraction, anchor"_L1;

[[nodiscard]] GraphFact factFrom(const QSqlQuery &query) {
    return {.id = query.value(0).toLongLong(),
            .node = query.value(1).toLongLong(),
            .kind = query.value(2).toString(),
            .value = query.value(3).toString(),
            .revealed = {placeFrom(query, 4), query.value(7).toString()}};
}
constexpr auto factColumns = "id, node_id, kind, value, COALESCE(book_id, 0), section, fraction, anchor"_L1;
// Facts in the order a reader comes to them: by book, then place, then as
// they were kept; those of no book after the rest.
constexpr auto factOrder = "COALESCE(book_id, 0) = 0, book_id, section, fraction, id"_L1;

// A book's place as a column takes it: no book is NULL, so a fact of no book
// is not tied to one, and a book's facts go with it when it is removed.
[[nodiscard]] QVariant bookColumn(qint64 bookId) { return bookId > 0 ? QVariant(bookId) : QVariant(QMetaType(QMetaType::LongLong)); }

// A few words of the text, as an anchor keeps them: on one line, and short.
[[nodiscard]] QString anchorText(const QString &words) { return words.simplified().left(160); }

// Later in the same book: a place further on, or the same place kept later.
[[nodiscard]] bool laterThan(const GraphFact &a, const GraphFact &b) {
    const GraphPlace &x = a.revealed.place, &y = b.revealed.place;
    return std::tuple{x.section, x.fraction, a.id} > std::tuple{y.section, y.fraction, b.id};
}

// "?, ?, ?" for as many values as there are.
[[nodiscard]] QString marks(qsizetype count) { return QStringList(count, "?").join(", "); }

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
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 id = named(scope, kind, called);
    // What it is newly said to be, and newly called, is kept as facts with
    // the moment it was revealed.
    const std::optional<GraphMoment> moment = revealed && revealed->place.bookId > 0 ? revealed
                                              : seen && seen->bookId > 0 ? std::optional(GraphMoment{*seen, anchorText(anchor)})
                                                                         : std::nullopt;
    QStringList newNames;
    if (id == 0) {
        const auto added = m_library->insert("INSERT INTO graph_nodes (scope, kind, name, summary, data, updated) VALUES (?, ?, ?, ?, ?, ?)",
                                             {scope, kind.simplified().toLower(), called, summary.trimmed(), compact(data), now});
        if (!added)
            return 0;
        id = *added;
        GraphNode made;
        made.data = data;
        newNames = made.aliases();
    } else {
        const GraphNode known = *node(id);
        QJsonObject merged = known.data;
        QStringList aliases = known.aliases();
        for (auto value = data.begin(); value != data.end(); ++value)
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
    // Revealed again in the same book: the earlier moment stands. Of no
    // book, the fact is kept anew as the latest word.
    QSqlQuery before = m_library->query(QString("SELECT %1 FROM graph_facts WHERE node_id = ? AND kind = ? AND value = ? "
                                                "AND COALESCE(book_id, 0) = ?").arg(factColumns),
                                        {node, kind, value, bookId});
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
        ->insert("INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction, anchor) VALUES (?, ?, ?, ?, ?, ?, ?)",
                 {node, kind, value, bookColumn(bookId), place.section, place.fraction, anchor})
        .value_or(0);
}

qint64 Graph::learn(qint64 id, const QString &kind, const QString &value, const std::optional<GraphMoment> &revealed) {
    const QString said = kind == "alias" ? value.simplified() : value.trimmed();
    const auto known = node(id);
    if (!known || said.isEmpty() || (kind != "summary" && kind != "alias"))
        return 0;
    if (kind == "summary") {
        m_library->exec("UPDATE graph_nodes SET summary = ?, updated = ? WHERE id = ?", {said, QDateTime::currentSecsSinceEpoch(), id});
    } else if (known->name.compare(said, Qt::CaseInsensitive) != 0 && !known->aliases().contains(said, Qt::CaseInsensitive)) {
        QJsonObject data = known->data;
        data.insert("aliases", QJsonArray::fromStringList(known->aliases() << said));
        m_library->exec("UPDATE graph_nodes SET data = ?, updated = ? WHERE id = ?", {compact(data), QDateTime::currentSecsSinceEpoch(), id});
    } else if (known->name.compare(said, Qt::CaseInsensitive) == 0) {
        return 0; // its own name is no other name
    }
    return recordFact(id, kind, said, revealed);
}

qint64 Graph::link(qint64 source, qint64 target, const QString &kind, const QString &note, const std::optional<GraphPlace> &learned,
                   const QString &anchor) {
    const QString relation = kind.simplified().toLower();
    if (source == 0 || target == 0 || source == target || relation.isEmpty() || !node(source) || !node(target))
        return 0;
    QSqlQuery existing = m_library->query(QString("SELECT %1 FROM graph_edges WHERE source = ? AND target = ? AND kind = ?").arg(edgeColumns),
                                          {source, target, relation});
    if (existing.next()) {
        const GraphEdge edge = edgeFrom(existing);
        const bool earlier = learned && learned->bookId > 0
                             && (edge.learned.bookId != learned->bookId || learned->notAfter(edge.learned));
        const GraphPlace place = earlier ? *learned : edge.learned;
        existing.finish();
        m_library->exec("UPDATE graph_edges SET note = ?, book_id = ?, section = ?, fraction = ?, anchor = ? WHERE id = ?",
                        {note.trimmed().isEmpty() ? edge.note : note.trimmed(), place.bookId, place.section, place.fraction,
                         earlier ? anchorText(anchor) : edge.anchor, edge.id});
        return edge.id;
    }
    const GraphPlace place = learned.value_or(GraphPlace());
    return m_library
        ->insert("INSERT INTO graph_edges (source, target, kind, note, book_id, section, fraction, anchor) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                 {source, target, relation, note.trimmed(), place.bookId, place.section, place.fraction,
                  place.bookId > 0 ? anchorText(anchor) : QString("")})
        .value_or(0);
}

bool Graph::remove(qint64 id) { return node(id) && m_library->exec("DELETE FROM graph_nodes WHERE id = ?", {id}); }
bool Graph::unlink(qint64 edge) { return m_library->exec("DELETE FROM graph_edges WHERE id = ?", {edge}); }

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
        link(edge.source == other ? keep : edge.source, edge.target == other ? keep : edge.target, edge.kind, edge.note,
             learnedIn(edge), edge.anchor);
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
    // What was said of it, with its moments, goes along; the summary of
    // now only where the kept node had none.
    m_library->exec("UPDATE graph_facts SET node_id = ? WHERE node_id = ? AND kind = 'summary'", {keep, other});
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
                         {called, summary.trimmed(), compact(data), QDateTime::currentSecsSinceEpoch(), id}))
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
    QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_nodes WHERE id = ?").arg(nodeColumns), {id});
    return query.next() ? std::optional(nodeFrom(query)) : std::nullopt;
}

QList<GraphPlace> Graph::seen(qint64 id) const {
    QList<GraphPlace> places;
    for (QSqlQuery query = m_library->query("SELECT book_id, section, fraction FROM graph_seen WHERE node_id = ? ORDER BY book_id", {id});
         query.next();)
        places.append(placeFrom(query, 0));
    return places;
}

std::optional<GraphMoment> Graph::firstMet(qint64 id, qint64 bookId) const {
    QSqlQuery query = m_library->query("SELECT book_id, section, fraction, anchor FROM graph_seen WHERE node_id = ? AND book_id = ?",
                                       {id, bookId});
    return query.next() ? std::optional(GraphMoment{placeFrom(query, 0), query.value(3).toString()}) : std::nullopt;
}

QList<GraphFact> Graph::facts(qint64 id) const { return factsOf({id}).value(id); }

QHash<qint64, QList<GraphFact>> Graph::factsOf(const QList<qint64> &nodes) const {
    QHash<qint64, QList<GraphFact>> facts;
    for (const auto batch : nodes | std::views::chunk(500))
        for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_facts WHERE node_id IN (%2) ORDER BY %3")
                                                    .arg(factColumns, marks(std::ranges::ssize(batch)), factOrder),
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

QList<GraphReveal> Graph::timeline(qint64 id, qint64 bookId) const {
    QList<GraphReveal> steps;
    const auto ofThis = [bookId](qint64 book) { return bookId == 0 || book == bookId || book == 0; };
    for (QSqlQuery query = m_library->query("SELECT book_id, section, fraction, anchor FROM graph_seen WHERE node_id = ?", {id});
         query.next();) {
        const GraphPlace place = placeFrom(query, 0);
        if (ofThis(place.bookId))
            steps.append({.what = "met", .text = {}, .revealed = {place, query.value(3).toString()}, .edge = 0, .other = 0,
                          .otherName = {}, .outgoing = true});
    }
    for (const GraphFact &fact : facts(id))
        if (ofThis(fact.revealed.place.bookId))
            steps.append({.what = fact.kind, .text = fact.value, .revealed = fact.revealed, .edge = 0, .other = 0, .otherName = {},
                          .outgoing = true});
    QHash<qint64, QString> names;
    for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_edges WHERE source = ? OR target = ?").arg(edgeColumns), {id, id});
         query.next();) {
        const GraphEdge edge = edgeFrom(query);
        // The book's own relations: what the library says of a book (who
        // wrote it, its series) is not something a reader comes to.
        if (edge.learned.bookId == 0 || !ofThis(edge.learned.bookId))
            continue;
        const qint64 other = edge.source == id ? edge.target : edge.source;
        if (!names.contains(other))
            names.insert(other, node(other).value_or(GraphNode()).name);
        steps.append({.what = "relation", .text = edge.relation(), .revealed = {edge.learned, edge.anchor}, .edge = edge.id,
                      .other = other, .otherName = names.value(other), .outgoing = edge.source == id});
    }
    // By book, then by place; where it was met before what was learned
    // there, and the facts of no book last.
    const auto order = [](const GraphReveal &step) {
        const GraphPlace &at = step.revealed.place;
        const int what = step.what == "met" ? 0 : step.what == "summary" ? 1 : step.what == "alias" ? 2 : 3;
        return std::tuple{at.bookId == 0, at.bookId, at.section, at.fraction, what};
    };
    std::ranges::stable_sort(steps, [&order](const GraphReveal &a, const GraphReveal &b) { return order(a) < order(b); });
    return steps;
}

QList<GraphNode> Graph::find(const QString &words, const QString &kind, const std::optional<QString> &scope, int limit) const {
    QString where = "1 = 1";
    QVariantList values;
    if (!kind.isEmpty()) {
        where += " AND kind = ?";
        values << kind.toLower();
    }
    if (scope) {
        where += " AND scope = ?";
        values << text(*scope);
    }
    for (const QString &word : words.split(' ', Qt::SkipEmptyParts)) {
        where += " AND (name LIKE ? OR summary LIKE ? OR data LIKE ?)";
        const QString pattern = "%" + word + "%";
        values << pattern << pattern << pattern;
    }
    return collect(where + " ORDER BY name COLLATE NOCASE", values, limit).nodes;
}

GraphView Graph::collect(const QString &nodesWhere, const QVariantList &values, int limit) const {
    GraphView view;
    for (QSqlQuery query = m_library->query(
             QString("SELECT %1 FROM graph_nodes WHERE %2 LIMIT %3").arg(nodeColumns, nodesWhere).arg(std::max(1, limit)), values);
         query.next();)
        view.nodes << nodeFrom(query);
    return view;
}

QList<GraphEdge> Graph::edgesAmong(const QList<GraphNode> &nodes) const {
    QList<GraphEdge> edges;
    const auto ids = nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QSet<qint64>>();
    // By source in batches, kept when the target is among the nodes too.
    const QList<qint64> all = ids.values();
    for (const auto batch : all | std::views::chunk(500)) {
        for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_edges WHERE source IN (%2) ORDER BY id")
                                                    .arg(edgeColumns, marks(std::ranges::ssize(batch))),
                                                batch | std::ranges::to<QVariantList>());
             query.next();)
            if (ids.contains(query.value(2).toLongLong()))
                edges << edgeFrom(query);
    }
    return edges;
}

GraphView Graph::around(qint64 start, int depth, int limit) const {
    GraphView view;
    QSet<qint64> seen{start};
    QList<qint64> frontier{start};
    for (int step = 0; step < std::clamp(depth, 0, 6) && !frontier.isEmpty() && seen.size() < limit; ++step) {
        QList<qint64> next;
        for (const qint64 id : std::as_const(frontier)) {
            QSqlQuery query = m_library->query("SELECT source, target FROM graph_edges WHERE source = ? OR target = ?", {id, id});
            while (query.next() && seen.size() < limit) {
                const qint64 other = query.value(0).toLongLong() == id ? query.value(1).toLongLong() : query.value(0).toLongLong();
                if (!seen.contains(other)) {
                    seen << other;
                    next << other;
                }
            }
        }
        frontier = std::move(next);
    }
    for (const qint64 id : std::as_const(seen))
        if (const auto found = node(id))
            view.nodes << *found;
    // The node asked about first, the rest in the order they were made.
    std::ranges::sort(view.nodes, [start](const GraphNode &a, const GraphNode &b) {
        return a.id == start || (b.id != start && a.id < b.id);
    });
    view.edges = edgesAmong(view.nodes);
    return view;
}

QList<std::pair<GraphNode, GraphPlace>> Graph::metIn(qint64 bookId, const QString &scope) const {
    QList<std::pair<GraphNode, GraphPlace>> met;
    for (QSqlQuery query = m_library->query(
             "SELECT graph_nodes.id, scope, kind, name, summary, data, COALESCE(graph_nodes.book_id, 0), updated, "
             "graph_seen.book_id, graph_seen.section, graph_seen.fraction FROM graph_nodes "
             "JOIN graph_seen ON graph_seen.node_id = graph_nodes.id WHERE graph_seen.book_id = ? AND scope = ? "
             "ORDER BY graph_seen.section, graph_seen.fraction, graph_nodes.id",
             {bookId, text(scope)});
         query.next();)
        met.append({nodeFrom(query), placeFrom(query, 8)});
    return met;
}

GraphView Graph::ofBook(qint64 bookId, const std::optional<GraphPlace> &upTo, const QString &scope) const {
    GraphView view;
    for (const auto &[node, place] : metIn(bookId, scope))
        if (!upTo || place.notAfter(*upTo))
            view.nodes << node;
    // Each as it was known there: its summary then, and the names it had.
    if (upTo) {
        const auto facts = factsOf(view.nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>());
        const GraphKnown known = [bookId, place = *upTo](const GraphPlace &at) { return at.bookId != bookId || at.notAfter(place); };
        for (GraphNode &node : view.nodes)
            node = asKnown(std::move(node), facts.value(node.id), bookId, known);
    }
    // What another book taught about the same things is part of the
    // picture; what this book teaches later is not, yet.
    std::ranges::copy_if(edgesAmong(view.nodes), std::back_inserter(view.edges), [bookId, &upTo](const GraphEdge &edge) {
        return edge.learned.bookId != bookId || !upTo || edge.learned.notAfter(*upTo);
    });
    return view;
}

GraphView Graph::beyond(qint64 bookId, const std::optional<GraphPlace> &upTo) const {
    GraphView view;
    QSet<qint64> taken;
    const auto own = ofBook(bookId, upTo).nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QSet<qint64>>();
    const QList<qint64> all = own.values();
    for (const auto batch : all | std::views::chunk(400)) {
        // Once for the sources and once for the targets.
        const QVariantList ids = batch | std::ranges::to<QVariantList>();
        QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_edges WHERE (source IN (%2) OR target IN (%2)) "
                                                   "AND (kind LIKE 'across: %' OR kind IN ('appears in', 'referred to in')) ORDER BY id")
                                               .arg(edgeColumns, marks(ids.size())),
                                           ids + ids);
        while (query.next()) {
            const GraphEdge edge = edgeFrom(query);
            const bool fromHere = own.contains(edge.source);
            if (fromHere == own.contains(edge.target))
                continue; // both ends are this book's: one of its own facts
            const auto other = node(fromHere ? edge.target : edge.source);
            if (!other)
                continue;
            if (edge.across()) {
                // Noticed in this book later than the place asked for.
                if (upTo && edge.learned.bookId == bookId && !edge.learned.notAfter(*upTo))
                    continue;
            } else if (other->kind != "book" || other->bookId == bookId) {
                continue; // a tie to this very book, not to another
            }
            view.edges << edge;
            if (!taken.contains(other->id)) {
                taken << other->id;
                view.nodes << *other;
            }
        }
    }
    return view;
}

GraphView Graph::acrossBooks(int limit) const {
    GraphView view;
    QSet<qint64> taken;
    QSqlQuery query = m_library->query(
        QString("SELECT %1 FROM graph_edges WHERE kind LIKE 'across: %' ORDER BY id DESC LIMIT %2").arg(edgeColumns).arg(std::max(1, limit)));
    while (query.next()) {
        const GraphEdge edge = edgeFrom(query);
        view.edges << edge;
        for (const qint64 id : {edge.source, edge.target})
            if (const auto end = taken.contains(id) ? std::nullopt : node(id)) {
                taken << id;
                view.nodes << *end;
            }
    }
    return view;
}

GraphView Graph::whole(const QString &scope, int limit) const {
    GraphView view = collect("scope = ? ORDER BY (SELECT COUNT(*) FROM graph_edges WHERE source = graph_nodes.id "
                             "OR target = graph_nodes.id) DESC, id",
                             {text(scope)}, limit);
    view.edges = edgesAmong(view.nodes);
    return view;
}

qint64 Graph::bookNode(qint64 bookId) {
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
    const qint64 now = QDateTime::currentSecsSinceEpoch();
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
    const Library::Batch batch(*m_library);
    for (const Book &book : m_library->books())
        bookNode(book.id);
    // Authors, series and tags no book points at any more are left over.
    m_library->exec("DELETE FROM graph_nodes WHERE scope = '' AND kind IN ('author', 'series', 'tag') AND id NOT IN "
                    "(SELECT source FROM graph_edges UNION SELECT target FROM graph_edges)");
}

QStringList Graph::scopes() const {
    QStringList scopes;
    for (QSqlQuery query = m_library->query("SELECT DISTINCT scope FROM graph_nodes ORDER BY scope"); query.next();)
        scopes << query.value(0).toString();
    return scopes;
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
    return days;
}
