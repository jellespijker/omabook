// The graph searched by its words (graph.h, #102): through its full-text
// index (GraphIndex), each search kept to the graph's tenant as every reader
// of it is (ADR 0003, #95).

#include <QJsonDocument>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <ranges>

#include "graph.h"
#include "graphindex.h"
#include "graphrows.h"

using namespace Qt::StringLiterals;
using namespace GraphRows;

namespace {

// The rows of the index that hold every phrase in these columns.
[[nodiscard]] QString matching(QStringView columns, const QStringList &phrases) {
    return u"{%1} : (%2)"_s.arg(columns.toString(), phrases.join(u" AND "_s));
}

// What a search looks in of a thing: its name, what is said of it, its data.
[[nodiscard]] QString textOf(const GraphNode &node) {
    return node.name + u'\n' + node.summary + u'\n' + QString::fromUtf8(QJsonDocument(node.data).toJson(QJsonDocument::Compact));
}

} // namespace

QList<GraphNode> Graph::find(const QString &words, const QString &kind, const std::optional<QString> &scope, int limit) const {
    QString where = u"1 = 1"_s;
    QVariantList values;
    if (!kind.isEmpty()) {
        where += u" AND kind = ?"_s;
        values << kind.toLower();
    }
    if (scope) {
        where += u" AND scope = ?"_s;
        values << text(*scope);
    }
    const QStringList asked = words.split(u' ', Qt::SkipEmptyParts);
    const QStringList phrases = asked | std::views::transform(GraphIndex::phrase)
                                | std::views::filter([](const auto &phrase) { return phrase.has_value(); })
                                | std::views::transform([](const auto &phrase) { return *phrase; }) | std::ranges::to<QStringList>();
    // A thing both tenants see is told as this one was told it (told()), which
    // the index doesn't hold apart: it is asked for what any tenant said of
    // it, and what this one was told is looked through again for the words.
    const bool tenanted = within().has_value() && !phrases.isEmpty();
    if (!phrases.isEmpty()) {
        where += u" AND id IN (SELECT rowid FROM graph_words WHERE graph_words MATCH ?)"_s;
        values << matching(tenanted ? u"name summary data said" : u"name summary data", phrases);
    }
    const int most = std::max(1, limit);
    // The few things the index found are each asked whether they are the
    // tenant's (has…), not the tenant's whole range read for them.
    const QString statement = u"SELECT %1 FROM graph_nodes WHERE (%2)%3 ORDER BY name COLLATE NOCASE, id LIMIT %4 OFFSET ?"_s.arg(
        nodeColumns, where, andWithin(phrases.isEmpty() ? &TenantFilter::nodes : &TenantFilter::hasNode, u"graph_nodes.id"),
        QString::number(most));
    QList<GraphNode> found;
    for (qsizetype offset = 0;; offset += most) {
        QList<GraphNode> page;
        for (QSqlQuery query = m_library->query(statement, values + QVariantList{offset}); query.next();)
            page << nodeFrom(query);
        const qsizetype read = page.size();
        found << (tenanted ? holding(std::move(page), phrases, asked) : page);
        if (!tenanted || read < most || found.size() >= most)
            break;
    }
    found.resize(std::min<qsizetype>(found.size(), most));
    told(found);
    return found;
}

QList<GraphNode> Graph::holding(QList<GraphNode> page, const QStringList &phrases, const QStringList &words) const {
    // Found by its own words, as against those of what was said of it once.
    const QList<qint64> ids = page | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>();
    QSet<qint64> byOwn;
    if (!ids.isEmpty())
        for (QSqlQuery query = m_library->query(
                 u"SELECT rowid FROM graph_words WHERE graph_words MATCH ? AND rowid IN (%1)"_s.arg(marks(std::ranges::ssize(ids))),
                 QVariantList{matching(u"name summary data", phrases)} + (ids | std::ranges::to<QVariantList>()));
             query.next();)
            byOwn << query.value(0).toLongLong();
    QList<GraphNode> asTold = page;
    told(asTold);
    QList<GraphNode> kept;
    for (const auto &[node, toldSo] : std::views::zip(page, asTold)) {
        const bool otherwise = toldSo.summary != node.summary || toldSo.data != node.data;
        if (otherwise ? GraphIndex::holds(textOf(toldSo), words) : byOwn.contains(node.id))
            kept << node;
    }
    return kept;
}

QSet<qint64> Graph::placedIn(const QList<qint64> &books) const {
    QSet<qint64> things;
    for (const auto batch : books | std::views::chunk(500)) {
        const QVariantList ids = batch | std::ranges::to<QVariantList>();
        const QString in = marks(std::ranges::ssize(batch));
        for (QSqlQuery query = m_library->query(u"SELECT node_id FROM graph_seen WHERE book_id IN (%1)%2"_s.arg(
                                                    in, andWithin(&TenantFilter::nodes, u"graph_seen.node_id")),
                                                ids);
             query.next();)
            things << query.value(0).toLongLong();
        for (QSqlQuery query = m_library->query(
                 u"SELECT node_id FROM graph_facts WHERE book_id IN (%1)%2"_s.arg(in, andWithin(&TenantFilter::facts, u"graph_facts.id")),
                 ids);
             query.next();)
            things << query.value(0).toLongLong();
        for (QSqlQuery query = m_library->query(u"SELECT source, target FROM graph_edges WHERE book_id IN (%1)%2"_s.arg(
                                                    in, andWithin(&TenantFilter::hasEdge, u"graph_edges.id")),
                                                ids);
             query.next();)
            things << query.value(0).toLongLong() << query.value(1).toLongLong();
    }
    return things;
}
