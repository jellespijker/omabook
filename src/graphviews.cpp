// The graph as it is read (graph.h): the views of it, search, and what is
// tied to the books, each kept to the graph's tenant when it has one (ADR
// 0003, #95). What writes the graph is in graph.cpp.

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

#include "graph.h"
#include "graphrows.h"

using namespace Qt::StringLiterals;
using namespace GraphRows;

void Graph::told(QList<GraphNode> &nodes) const {
    const std::optional<TenantFilter> &filter = within();
    if (!filter || nodes.isEmpty())
        return;
    // What another tenant was told of these things: only those things are
    // told again, by what this tenant was told.
    QHash<qint64, QList<GraphFact>> elsewhere;
    const QList<qint64> ids = nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>();
    for (const auto batch : ids | std::views::chunk(500))
        for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_facts AS f WHERE f.node_id IN (%2) AND f.kind IN ('summary', "
                                                        "'alias') AND NOT %3")
                                                    .arg(factColumns, marks(std::ranges::ssize(batch)), filter->hasFact(u"f.id")),
                                                batch | std::ranges::to<QVariantList>());
             query.next();) {
            GraphFact fact = factFrom(query);
            elsewhere[fact.node].append(std::move(fact));
        }
    if (elsewhere.isEmpty())
        return;
    const QHash<qint64, QList<GraphFact>> here = factsOf(elsewhere.keys());
    // Another tenant's facts are kept at no place any reader is at, so that
    // asKnown leaves them out as it leaves out what is past a reader's page:
    // its summary the latest this tenant was told, and only its names.
    const GraphKnown known = [](const GraphPlace &at) { return at.bookId >= 0; };
    for (GraphNode &node : nodes) {
        const auto theirs = elsewhere.constFind(node.id);
        if (theirs == elsewhere.cend())
            continue;
        QList<GraphFact> all = here.value(node.id);
        for (GraphFact fact : *theirs) {
            fact.revealed.place.bookId = -1;
            all << fact;
        }
        node = asKnown(std::move(node), all, 0, known);
    }
}

QList<Graph::Sighting> Graph::sightings(const QStringList &kinds) const {
    QList<Sighting> found;
    if (kinds.isEmpty())
        return found;
    const QString kindList = marks(kinds.size());
    const QVariantList kindValues = kinds | std::ranges::to<QVariantList>();
    const QString columns = "n.id, n.scope, n.kind, n.name, n.summary, n.data, COALESCE(n.book_id, 0), n.updated"_L1;
    // Each node made once, however many books it is tied to, and of its
    // data only its other names, which is all a search looks at.
    QHash<qint64, GraphNode> nodes;
    const auto nodeOf = [&nodes](const QSqlQuery &query) {
        const qint64 id = query.value(0).toLongLong();
        if (const auto known = nodes.constFind(id); known != nodes.cend())
            return *known;
        GraphNode node;
        node.id = id;
        node.scope = query.value(1).toString();
        node.kind = query.value(2).toString();
        node.name = query.value(3).toString();
        node.summary = query.value(4).toString();
        if (const QString data = query.value(5).toString(); data.contains("\"aliases\""_L1))
            node.data = QJsonObject{{"aliases", QJsonDocument::fromJson(data.toUtf8()).object().value("aliases")}};
        node.bookId = query.value(6).toLongLong();
        node.updated = query.value(7).toLongLong();
        return *nodes.insert(id, node);
    };
    // Met in a book, through the primary key of graph_seen from each node.
    for (QSqlQuery query = m_library->query(QString("SELECT %1, s.book_id, s.section, s.fraction, s.anchor FROM graph_nodes AS n "
                                                    "JOIN graph_seen AS s ON s.node_id = n.id WHERE n.scope = '' AND n.kind IN (%2)%3")
                                                .arg(columns, kindList, andWithin(&TenantFilter::books, u"s.book_id")),
                                            kindValues);
         query.next();)
        found.append({nodeOf(query), query.value(8).toLongLong(), {placeFrom(query, 8), query.value(11).toString()}});
    // Related to a book's own node, either way round.
    for (const auto &[thing, book] : {std::pair{"source"_L1, "target"_L1}, std::pair{"target"_L1, "source"_L1}})
        for (QSqlQuery query =
                 m_library->query(QString("SELECT %1, b.book_id, e.book_id, e.section, e.fraction, e.anchor FROM graph_edges AS e "
                                          "JOIN graph_nodes AS n ON n.id = e.%2 JOIN graph_nodes AS b ON b.id = e.%3 "
                                          "WHERE b.kind = 'book' AND b.book_id IS NOT NULL AND n.scope = '' AND n.kind IN (%4)%5%6")
                                      .arg(columns, thing, book, kindList, andWithin(&TenantFilter::books, u"b.book_id"),
                                           andWithin(&TenantFilter::hasEdge, u"e.id")),
                                  kindValues);
             query.next();)
            found.append({nodeOf(query), query.value(8).toLongLong(), {placeFrom(query, 9), query.value(12).toString()}});
    // A question asked of a book with no place in it.
    for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_nodes AS n WHERE n.scope = '' AND n.kind = 'question' "
                                                    "AND n.data LIKE '%\"book\"%' AND NOT EXISTS (SELECT 1 FROM graph_seen "
                                                    "WHERE node_id = n.id)%2")
                                                .arg(columns, andWithin(&TenantFilter::nodes, u"n.id")));
         query.next();) {
        GraphNode node = nodeFrom(query);
        if (const qint64 book = node.data.value("book").toInteger(); book > 0 && kinds.contains(node.kind) && sees(book))
            found.append({std::move(node), book, {}});
    }
    // Each as the tenant knows it.
    QList<GraphNode> unique = nodes.values();
    told(unique);
    for (const GraphNode &node : std::as_const(unique))
        nodes.insert(node.id, node);
    for (Sighting &sighting : found)
        sighting.node = nodes.value(sighting.node.id, sighting.node);
    return found;
}

QHash<qint64, QList<GraphFact>> Graph::factsOfKinds(const QStringList &kinds) const {
    QHash<qint64, QList<GraphFact>> facts;
    if (kinds.isEmpty())
        return facts;
    for (QSqlQuery query = m_library->query(
             QString("SELECT f.id, f.node_id, f.kind, f.value, COALESCE(f.book_id, 0), f.section, f.fraction, f.anchor "
                     "FROM graph_facts AS f JOIN graph_nodes AS n ON n.id = f.node_id WHERE n.scope = '' AND n.kind IN (%1)%2")
                 .arg(marks(kinds.size()), andWithin(&TenantFilter::facts, u"f.id")),
             kinds | std::ranges::to<QVariantList>());
         query.next();) {
        GraphFact fact = factFrom(query);
        facts[fact.node].append(std::move(fact));
    }
    return facts;
}

QList<GraphReveal> Graph::timeline(qint64 id, qint64 bookId) const {
    QList<GraphReveal> steps;
    const auto ofThis = [bookId](qint64 book) { return bookId == 0 || book == bookId || book == 0; };
    for (QSqlQuery query = m_library->query(QString("SELECT book_id, section, fraction, anchor FROM graph_seen WHERE node_id = ?%1")
                                                .arg(andWithin(&TenantFilter::hasBook, u"graph_seen.book_id")),
                                            {id});
         query.next();) {
        const GraphPlace place = placeFrom(query, 0);
        if (ofThis(place.bookId))
            steps.append({.what = "met",
                          .text = {},
                          .revealed = {place, query.value(3).toString()},
                          .edge = 0,
                          .other = 0,
                          .otherName = {},
                          .outgoing = true});
    }
    for (const GraphFact &fact : facts(id))
        if (ofThis(fact.revealed.place.bookId))
            steps.append({.what = fact.kind,
                          .text = fact.value,
                          .revealed = fact.revealed,
                          .edge = 0,
                          .other = 0,
                          .otherName = {},
                          .outgoing = true});
    QHash<qint64, QString> names;
    for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_edges WHERE (source = ? OR target = ?)%2")
                                                .arg(edgeColumns, andWithin(&TenantFilter::hasEdge, u"graph_edges.id")),
                                            {id, id});
         query.next();) {
        const GraphEdge edge = edgeFrom(query);
        // The book's own relations: what the library says of a book (who
        // wrote it, its series) is not something a reader comes to.
        if (edge.learned.bookId == 0 || !ofThis(edge.learned.bookId))
            continue;
        const qint64 other = edge.source == id ? edge.target : edge.source;
        if (!names.contains(other))
            names.insert(other, node(other).value_or(GraphNode()).name);
        steps.append({.what = "relation",
                      .text = edge.relation(),
                      .revealed = {edge.learned, edge.anchor},
                      .edge = edge.id,
                      .other = other,
                      .otherName = names.value(other),
                      .outgoing = edge.source == id});
    }
    // By book, then by place; where it was met before what was learned
    // there, and the facts of no book last.
    const auto order = [](const GraphReveal &step) {
        const GraphPlace &at = step.revealed.place;
        static const QStringList firsts{"met", "summary", "alias"}; // then the relations
        const qsizetype index = firsts.indexOf(step.what);
        const int what = static_cast<int>(index < 0 ? firsts.size() : index);
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
    return collect(where, values, limit, "name COLLATE NOCASE").nodes;
}

GraphView Graph::collect(const QString &nodesWhere, const QVariantList &values, int limit, const QString &order) const {
    GraphView view;
    for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_nodes WHERE (%2)%3 ORDER BY %4 LIMIT %5")
                                                .arg(nodeColumns, nodesWhere, andWithin(&TenantFilter::nodes, u"graph_nodes.id"), order)
                                                .arg(std::max(1, limit)),
                                            values);
         query.next();)
        view.nodes << nodeFrom(query);
    told(view.nodes);
    return view;
}

QList<GraphEdge> Graph::edgesAmong(const QList<GraphNode> &nodes) const {
    QList<GraphEdge> edges;
    const auto ids = nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QSet<qint64>>();
    // By source in batches, kept when the target is among the nodes too.
    const QList<qint64> all = ids.values();
    for (const auto batch : all | std::views::chunk(500)) {
        for (QSqlQuery query = m_library->query(
                 QString("SELECT %1 FROM graph_edges WHERE source IN (%2)%3 ORDER BY id")
                     .arg(edgeColumns, marks(std::ranges::ssize(batch)), andWithin(&TenantFilter::hasEdge, u"graph_edges.id")),
                 batch | std::ranges::to<QVariantList>());
             query.next();)
            if (ids.contains(query.value(2).toLongLong()))
                edges << edgeFrom(query);
    }
    std::ranges::sort(edges, {}, &GraphEdge::id);
    told(edges);
    return edges;
}

GraphView Graph::around(qint64 start, int depth, int limit) const {
    GraphView view;
    QSet<qint64> seen{start};
    QList<qint64> frontier{start};
    const QString edges = QString("SELECT source, target FROM graph_edges WHERE (source = ? OR target = ?)%1")
                              .arg(andWithin(&TenantFilter::hasEdge, u"graph_edges.id"));
    for (int step = 0; step < std::clamp(depth, 0, 6) && !frontier.isEmpty() && seen.size() < limit; ++step) {
        QList<qint64> next;
        for (const qint64 id : std::as_const(frontier)) {
            QSqlQuery query = m_library->query(edges, {id, id});
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
    std::ranges::sort(view.nodes,
                      [start](const GraphNode &a, const GraphNode &b) { return a.id == start || (b.id != start && a.id < b.id); });
    view.edges = edgesAmong(view.nodes);
    return view;
}

QList<std::pair<GraphNode, GraphPlace>> Graph::metIn(qint64 bookId, const QString &scope) const {
    QList<std::pair<GraphNode, GraphPlace>> met;
    for (QSqlQuery query =
             m_library->query(QString("SELECT graph_nodes.id, scope, kind, name, summary, data, COALESCE(graph_nodes.book_id, 0), updated, "
                                      "graph_seen.book_id, graph_seen.section, graph_seen.fraction FROM graph_nodes "
                                      "JOIN graph_seen ON graph_seen.node_id = graph_nodes.id WHERE graph_seen.book_id = ? AND scope = ?%1 "
                                      "ORDER BY graph_seen.section, graph_seen.fraction, graph_nodes.id")
                                  .arg(andWithin(&TenantFilter::hasBook, u"graph_seen.book_id")),
                              {bookId, text(scope)});
         query.next();)
        met.append({nodeFrom(query), placeFrom(query, 8)});
    QList<GraphNode> nodes = met | std::views::transform([](const auto &one) { return one.first; }) | std::ranges::to<QList<GraphNode>>();
    told(nodes);
    for (qsizetype at = 0; at < met.size(); ++at)
        met[at].first = nodes.at(at);
    return met;
}

GraphView Graph::ofBook(qint64 bookId, const std::optional<GraphPlace> &upTo, const QString &scope) const {
    GraphView view;
    for (const auto &[node, place] : metIn(bookId, scope))
        if (!upTo || place.notAfter(*upTo))
            view.nodes << node;
    // Each as it was known there: its summary then, and the names it had.
    // What another tenant's book taught is not read at all (within()).
    if (upTo) {
        const auto facts = factsOf(view.nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>());
        const GraphKnown known = [bookId, place = *upTo](const GraphPlace &at) { return at.bookId != bookId || at.notAfter(place); };
        for (GraphNode &node : view.nodes) {
            const qint64 id = node.id;
            node = asKnown(std::move(node), facts.value(id), bookId, known);
        }
    }
    // What another book of the tenant taught about the same things is part
    // of the picture; what this book teaches later is not, yet.
    std::ranges::copy_if(edgesAmong(view.nodes), std::back_inserter(view.edges), [bookId, &upTo](const GraphEdge &edge) {
        return edge.learned.bookId != bookId || !upTo || edge.learned.notAfter(*upTo);
    });
    return view;
}

GraphView Graph::beyond(qint64 bookId, const std::optional<GraphPlace> &upTo) const {
    const auto own = ofBook(bookId, upTo).nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QSet<qint64>>();
    const QList<qint64> all = own.values();
    QList<std::pair<GraphEdge, GraphNode>> ties;
    for (const auto batch : all | std::views::chunk(400)) {
        // Once for the sources and once for the targets.
        const QVariantList ids = batch | std::ranges::to<QVariantList>();
        QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_edges WHERE (source IN (%2) OR target IN (%2)) "
                                                   "AND (kind LIKE 'across: %' OR kind IN ('appears in', 'referred to in'))%3 ORDER BY id")
                                               .arg(edgeColumns, marks(ids.size()), andWithin(&TenantFilter::hasEdge, u"graph_edges.id")),
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
            } else if (other->kind != "book" || other->bookId == bookId || !sees(other->bookId)) {
                continue; // a tie to this very book, not to another of the tenant
            }
            ties.append({edge, *other});
        }
    }
    GraphView view;
    QSet<qint64> taken;
    for (const auto &[edge, other] : std::as_const(ties)) {
        view.edges << edge;
        if (!taken.contains(other.id)) {
            taken << other.id;
            view.nodes << other;
        }
    }
    told(view.edges);
    return view;
}

GraphView Graph::acrossBooks(int limit) const {
    GraphView view;
    QSet<qint64> taken;
    for (QSqlQuery query = m_library->query(QString("SELECT %1 FROM graph_edges WHERE kind LIKE 'across: %'%2 ORDER BY id DESC LIMIT %3")
                                                .arg(edgeColumns, andWithin(&TenantFilter::edges, u"graph_edges.id"))
                                                .arg(std::max(1, limit)));
         query.next();) {
        const GraphEdge edge = edgeFrom(query);
        view.edges << edge;
        for (const qint64 id : {edge.source, edge.target})
            if (const auto end = taken.contains(id) ? std::nullopt : node(id)) {
                taken << id;
                view.nodes << *end;
            }
    }
    told(view.edges);
    return view;
}

GraphView Graph::whole(const QString &scope, int limit) const {
    const std::optional<TenantFilter> &filter = within();
    if (!filter) {
        // The whole graph, of no tenant, by every relation each thing has.
        GraphView view = collect("scope = ?", {text(scope)}, limit,
                                 "(SELECT COUNT(*) FROM graph_edges WHERE source = graph_nodes.id OR target = graph_nodes.id) DESC, id");
        view.edges = edgesAmong(view.nodes);
        return view;
    }
    // The most connected first, by the relations the tenant sees: its own
    // and those of what every tenant sees (tenant 0), each by the degree kept
    // there (#101). A thing is in one or the other, never both
    // (TenancySchema), so the two are merged, not added.
    const bool general = text(scope).isEmpty();
    QString sql = mostConnected(0, general, limit);
    QVariantList values{text(scope)};
    if (filter->id() > 0) {
        sql = QString("SELECT * FROM (%1) UNION ALL SELECT * FROM (%2) ORDER BY 9 DESC, 1 LIMIT %3")
                  .arg(mostConnected(filter->id(), general, limit), sql)
                  .arg(std::max(1, limit));
        values << text(scope);
    }
    GraphView view;
    for (QSqlQuery query = m_library->query(sql, values); query.next();)
        view.nodes << nodeFrom(query);
    told(view.nodes);
    view.edges = edgesAmong(view.nodes);
    return view;
}

QStringList Graph::scopes() const {
    QStringList scopes;
    for (QSqlQuery query = m_library->query("SELECT DISTINCT scope FROM graph_nodes WHERE 1 = 1"
                                            + andWithin(&TenantFilter::nodes, u"graph_nodes.id") + " ORDER BY scope");
         query.next();)
        scopes << query.value(0).toString();
    return scopes;
}
