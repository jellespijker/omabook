#include "changeloggraph.h"

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlQuery>
#include <QStringList>
#include <optional>
#include <utility>

#include "changelog.h"
#include "changelogfields.h"
#include "hlc.h"
#include "spacereplay.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

using FieldMap = ChangeLogFields::Map;
using GraphContext = ChangeLogFields::Context;

[[nodiscard]] QString compactJson(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void syncNodeRowToLog(const Library &library, const QSqlQuery &query, bool baseline, std::optional<qint64> explicitHlc = std::nullopt) {
    const qint64 id = query.value(0).toLongLong();
    const QString scope = query.value(1).toString();
    const QString gid = Spaces::nodeId(library, id);
    GraphContext ctx{&library, Spaces::spaceForScope(library, scope), u"node"_s, gid, ChangeLogFields::logged(library, u"node"_s, gid),
                     baseline};
    const qint64 hlc = explicitHlc ? Hlc::normalize(*explicitHlc) : Hlc::normalize(query.value(7).toLongLong());
    const qint64 bookId = query.value(6).toLongLong();
    for (const auto &[field, col] : {std::pair{u"scope"_s, 1}, {u"kind"_s, 2}, {u"name"_s, 3}, {u"summary"_s, 4}, {u"data"_s, 5}})
        ChangeLogFields::write(ctx, field, query.value(col).toString(), hlc);
    ChangeLogFields::write(ctx, u"book"_s, bookId > 0 ? Spaces::bookId(library, bookId) : QString(), hlc);
    if (ctx.existing.contains(u"removed"_s) && ctx.existing.value(u"removed"_s).value == "1"_L1)
        ChangeLog::append(library, {ctx.space, u"node"_s, gid, u"removed"_s, u"0"_s, Spaces::tickHlc(library)});
    for (QSqlQuery seen = ChangeLog::query(
             library, "SELECT book_id, section, fraction, anchor FROM graph_seen WHERE node_id = ? ORDER BY book_id", {id});
         seen.next();) {
        const QString bookGid = Spaces::bookId(library, seen.value(0).toLongLong());
        if (!bookGid.isEmpty())
            ChangeLogFields::write(
                ctx, u"seen:"_s + bookGid,
                compactJson(
                    {{"section", seen.value(1).toInt()}, {"fraction", seen.value(2).toDouble()}, {"anchor", seen.value(3).toString()}}),
                hlc);
    }
    if (explicitHlc && SpaceReplay::entityHlc(library, u"node"_s, gid) < hlc)
        ChangeLog::append(library, {ctx.space, u"node"_s, gid, u"name"_s, query.value(3).toString(), hlc});
}

// " AND x IN (1, 2, 3)" for these ids, or nothing for all of them. The ids are
// numbers from the database, so they go into the statement as they are.
[[nodiscard]] QString onlyThese(const QString &column, const std::optional<QList<qint64>> &ids) {
    if (!ids)
        return {};
    QStringList listed;
    for (const qint64 id : *ids)
        listed << QString::number(id);
    return u" AND "_s + column + u" IN ("_s + (listed.isEmpty() ? u"NULL"_s : listed.join(u',')) + u")"_s;
}

void syncFactsToLog(const Library &library, const std::optional<QList<qint64>> &ids = std::nullopt) {
    if (ids && ids->isEmpty())
        return;
    const auto makeCtx = [&](const QString &space, const QString &entity, const QString &gid) {
        return GraphContext{&library, space, entity, gid, ChangeLogFields::logged(library, entity, gid), true};
    };
    for (QSqlQuery query =
             ChangeLog::query(library, u"SELECT f.id, f.node_id, f.kind, f.value, COALESCE(f.book_id, 0), f.section, f.fraction, f.anchor, "
                                       "n.scope, n.updated FROM graph_facts AS f JOIN graph_nodes AS n ON n.id = f.node_id WHERE 1"_s
                                           + onlyThese(u"f.id"_s, ids) + u" ORDER BY f.id"_s);
         query.next();) {
        const QString gid = Spaces::factId(library, query.value(0).toLongLong());
        const qint64 bookId = query.value(4).toLongLong();
        const QString space = Spaces::spaceForScope(library, query.value(8).toString());
        GraphContext ctx = makeCtx(space, u"fact"_s, gid);
        ChangeLogFields::write(ctx, u"revealed"_s,
                               compactJson({{"node", Spaces::nodeId(library, query.value(1).toLongLong())},
                                            {"kind", query.value(2).toString()},
                                            {"value", query.value(3).toString()},
                                            {"book", bookId > 0 ? Spaces::bookId(library, bookId) : QString()},
                                            {"section", query.value(5).toInt()},
                                            {"fraction", query.value(6).toDouble()},
                                            {"anchor", query.value(7).toString()}}),
                               Hlc::normalize(query.value(9).toLongLong()));
    }
}

void syncEdgesToLog(const Library &library, const std::optional<QList<qint64>> &ids = std::nullopt) {
    if (ids && ids->isEmpty())
        return;
    for (QSqlQuery query =
             ChangeLog::query(library, u"SELECT e.id, e.source, e.target, e.kind, e.note, e.book_id, e.section, e.fraction, e.anchor, "
                                       "n.scope, n.updated FROM graph_edges AS e JOIN graph_nodes AS n ON n.id = e.source WHERE 1"_s
                                           + onlyThese(u"e.id"_s, ids) + u" ORDER BY e.id"_s);
         query.next();) {
        const QString gid = Spaces::edgeId(library, query.value(0).toLongLong());
        const qint64 bookId = query.value(5).toLongLong();
        const QString space = Spaces::spaceForScope(library, query.value(9).toString());
        GraphContext ctx{&library, space, u"edge"_s, gid, ChangeLogFields::logged(library, u"edge"_s, gid), true};
        ChangeLogFields::write(ctx, u"link"_s,
                               compactJson({{"source", Spaces::nodeId(library, query.value(1).toLongLong())},
                                            {"target", Spaces::nodeId(library, query.value(2).toLongLong())},
                                            {"kind", query.value(3).toString()},
                                            {"note", query.value(4).toString()},
                                            {"book", bookId > 0 ? Spaces::bookId(library, bookId) : QString()},
                                            {"section", query.value(6).toInt()},
                                            {"fraction", query.value(7).toDouble()},
                                            {"anchor", query.value(8).toString()}}),
                               Hlc::normalize(query.value(10).toLongLong()));
        if (ctx.existing.contains(u"removed"_s) && ctx.existing.value(u"removed"_s).value == "1"_L1)
            ChangeLog::append(library, {space, u"edge"_s, gid, u"removed"_s, u"0"_s, Spaces::tickHlc(library)});
    }
}

void syncNodesToLog(const Library &library, bool baseline, const std::optional<QList<qint64>> &ids) {
    if (ids && ids->isEmpty())
        return;
    for (QSqlQuery query = ChangeLog::query(library, u"SELECT id, scope, kind, name, summary, data, COALESCE(book_id, 0), updated "
                                                     "FROM graph_nodes WHERE 1"_s
                                                         + onlyThese(u"id"_s, ids) + u" ORDER BY id"_s);
         query.next();)
        syncNodeRowToLog(library, query, baseline);
}

void syncReadingLogToLog(const Library &library, const std::optional<QList<qint64>> &books) {
    if (books && books->isEmpty())
        return;
    for (QSqlQuery query = ChangeLog::query(library, u"SELECT book_id, day, from_percent, to_percent FROM reading_log WHERE 1"_s
                                                         + onlyThese(u"book_id"_s, books) + u" ORDER BY book_id, day"_s);
         query.next();) {
        const QString bookGid = Spaces::bookId(library, query.value(0).toLongLong());
        if (bookGid.isEmpty())
            continue;
        const QString day = query.value(1).toString();
        const QString gid = bookGid + u":"_s + day;
        const QString space = Spaces::personalSpace(library);
        GraphContext ctx{&library, space, u"reading_log"_s, gid, ChangeLogFields::logged(library, u"reading_log"_s, gid), true};
        ChangeLogFields::write(
            ctx, u"range"_s,
            compactJson({{"book", bookGid}, {"day", day}, {"from", query.value(2).toDouble()}, {"to", query.value(3).toDouble()}}),
            Spaces::lastHlc(library));
    }
}

// A thing, fact or relation that is gone: its tombstone, unless it has one.
void tombstone(const Library &library, const QString &entity, const QString &gid, const QString &space) {
    if (gid.isEmpty())
        return;
    const FieldMap existing = ChangeLogFields::logged(library, entity, gid);
    if (!existing.isEmpty() && (!existing.contains(u"removed"_s) || existing.value(u"removed"_s).value != "1"_L1))
        ChangeLog::append(library, {space, entity, gid, u"removed"_s, u"1"_s, Spaces::tickHlc(library)});
}

[[nodiscard]] bool exists(const Library &library, const QString &table, qint64 id) {
    QSqlQuery query = ChangeLog::query(library, u"SELECT 1 FROM "_s + table + u" WHERE id = ?"_s, {id});
    return query.next();
}

} // namespace

void ChangeLogGraph::syncToLog(const Library &library, bool baseline) {
    syncNodesToLog(library, baseline, std::nullopt);
    syncFactsToLog(library);
    syncEdgesToLog(library);
    syncReadingLogToLog(library, std::nullopt);
}

void ChangeLogGraph::syncNodeAt(const Library &library, qint64 nodeId, qint64 hlc) {
    const ChangeLog::Scope guard;
    const qint64 normalized = Hlc::normalize(hlc);
    if (normalized > 0)
        Spaces::observeHlc(library, normalized);
    for (QSqlQuery query = ChangeLog::query(
             library, "SELECT id, scope, kind, name, summary, data, COALESCE(book_id, 0), updated FROM graph_nodes WHERE id = ?", {nodeId});
         query.next();)
        syncNodeRowToLog(library, query, true, normalized);
    ChangeLog::exec(library, "DELETE FROM log_dirty WHERE entity = 'node' AND local_id = ?", {nodeId});
}

void ChangeLogGraph::syncRowsToLog(const Library &library, const Rows &rows) {
    syncNodesToLog(library, false, rows.nodes);
    syncFactsToLog(library, rows.facts);
    syncEdgesToLog(library, rows.edges);
    syncReadingLogToLog(library, rows.readingBooks);
    // What is no longer there; its space is the one its operations went to.
    const auto gone = [&](const QString &entity, const QString &table, const QList<qint64> &ids, const auto &globalId) {
        for (const qint64 id : ids) {
            if (exists(library, table, id))
                continue;
            const QString gid = globalId(library, id);
            QSqlQuery space = ChangeLog::query(library,
                                               "SELECT s.name FROM ops AS o JOIN log_ids AS s ON s.id = o.space "
                                               "WHERE o.entity = ? AND o.id = (SELECT id FROM log_ids WHERE name = ?) LIMIT 1",
                                               {entity, gid});
            if (space.next())
                tombstone(library, entity, gid, space.value(0).toString());
        }
    };
    gone(u"node"_s, u"graph_nodes"_s, rows.nodes, Spaces::nodeId);
    gone(u"fact"_s, u"graph_facts"_s, rows.facts, Spaces::factId);
    gone(u"edge"_s, u"graph_edges"_s, rows.edges, Spaces::edgeId);
}

void ChangeLogGraph::detectDeletedEntity(const Library &library, const QString &entity, const QString &table) {
    QSet<QString> alive;
    for (QSqlQuery query = ChangeLog::query(library, "SELECT id FROM " + table); query.next();) {
        const qint64 id = query.value(0).toLongLong();
        if (entity == "node"_L1)
            alive.insert(Spaces::nodeId(library, id));
        else if (entity == "edge"_L1)
            alive.insert(Spaces::edgeId(library, id));
        else
            alive.insert(Spaces::factId(library, id));
    }
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT DISTINCT s.name, i.name FROM ops AS o JOIN log_ids AS s ON s.id = o.space "
                                            "JOIN log_ids AS i ON i.id = o.id WHERE o.entity = ?",
                                            {entity});
         query.next();) {
        const QString space = query.value(0).toString();
        const QString gid = query.value(1).toString();
        if (alive.contains(gid))
            continue;
        const FieldMap existing = ChangeLogFields::logged(library, entity, gid);
        if (!existing.contains(u"removed"_s) || existing.value(u"removed"_s).value != "1"_L1)
            ChangeLog::append(library, {space, entity, gid, u"removed"_s, u"1"_s, Spaces::tickHlc(library)});
    }
}
