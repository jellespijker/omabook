#pragma once

#include <QJsonDocument>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <algorithm>

#include "graph.h"

// How the graph's rows read into its types, for the two halves of Graph:
// what writes it (graph.cpp) and what reads it as a tenant sees it
// (graphviews.cpp), and what keeps a relation's notes (graphnotes.cpp). Each reads the columns its *Columns names, in order.
namespace GraphRows {

// A null string binds as SQL NULL, which a scope, a summary and a note may not be.
[[nodiscard]] inline QString text(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }

[[nodiscard]] inline GraphNode nodeFrom(const QSqlQuery &query) {
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
constexpr QLatin1StringView nodeColumns{"id, scope, kind, name, summary, data, COALESCE(book_id, 0), updated"};

// A few words of the text, as an anchor keeps them: on one line, and short.
[[nodiscard]] inline QString anchorText(const QString &words) { return words.simplified().left(160); }

// A place as three columns of a row hold it, the first of them at `column`.
[[nodiscard]] inline GraphPlace placeFrom(const QSqlQuery &query, int column) {
    return {query.value(column).toLongLong(), query.value(column + 1).toInt(), query.value(column + 2).toDouble()};
}

[[nodiscard]] inline GraphEdge edgeFrom(const QSqlQuery &query) {
    return {.id = query.value(0).toLongLong(),
            .source = query.value(1).toLongLong(),
            .target = query.value(2).toLongLong(),
            .kind = query.value(3).toString(),
            .note = query.value(4).toString(),
            .learned = placeFrom(query, 5),
            .anchor = query.value(8).toString()};
}
constexpr QLatin1StringView edgeColumns{"id, source, target, kind, note, book_id, section, fraction, anchor"};

[[nodiscard]] inline GraphFact factFrom(const QSqlQuery &query) {
    return {.id = query.value(0).toLongLong(),
            .node = query.value(1).toLongLong(),
            .kind = query.value(2).toString(),
            .value = query.value(3).toString(),
            .revealed = {placeFrom(query, 4), query.value(7).toString()}};
}
constexpr QLatin1StringView factColumns{"id, node_id, kind, value, COALESCE(book_id, 0), section, fraction, anchor"};
// Facts in the order a reader comes to them: by book, then place, then as
// they were kept; those of no book after the rest.
constexpr QLatin1StringView factOrder{"COALESCE(book_id, 0) = 0, book_id, section, fraction, id"};

// The things of a scope that `tenant` (its number, TenantFilter::id) sees,
// the most connected first by their degree there (tenant_nodes.degree), and
// cut at `limit`; with a scope's value to bind once. In the general graph,
// which holds most of a tenant's things, a range of the index that the
// tenant and the degree lead, stopped at the limit; in a world, its few
// things by the scope, then sorted. Each thing's degree after its columns.
[[nodiscard]] inline QString mostConnected(qint64 tenant, bool general, int limit) {
    const QString columns = QStringLiteral("n.id, n.scope, n.kind, n.name, n.summary, n.data, COALESCE(n.book_id, 0), n.updated, m.degree");
    const QString from = general ? QStringLiteral("tenant_nodes AS m CROSS JOIN graph_nodes AS n ON n.id = m.node_id WHERE m.tenant = %1")
                                 : QStringLiteral("graph_nodes AS n CROSS JOIN tenant_nodes AS m ON m.tenant = %1 AND m.node_id = n.id WHERE 1");
    return QStringLiteral("SELECT %1 FROM %2 AND n.scope = ? ORDER BY m.degree DESC, m.node_id LIMIT %3")
        .arg(columns, from.arg(tenant))
        .arg(std::max(1, limit));
}

// "?, ?, ?" for as many values as there are.
[[nodiscard]] inline QString marks(qsizetype count) { return QStringList(count, QStringLiteral("?")).join(QStringLiteral(", ")); }

} // namespace GraphRows
