#pragma once

#include <QJsonDocument>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "graph.h"

// How the graph's rows read into its types, for the two halves of Graph:
// what writes it (graph.cpp) and what reads it as a tenant sees it
// (graphviews.cpp). Each reads the columns its *Columns names, in order.
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

// A place as three columns of a row hold it, the first of them at `column`.
[[nodiscard]] inline GraphPlace placeFrom(const QSqlQuery &query, int column) {
    return {.bookId = query.value(column).toLongLong(),
            .section = query.value(column + 1).toInt(),
            .fraction = query.value(column + 2).toDouble()};
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

// "?, ?, ?" for as many values as there are.
[[nodiscard]] inline QString marks(qsizetype count) { return QStringList(count, QStringLiteral("?")).join(QStringLiteral(", ")); }

} // namespace GraphRows
