#include "changelog.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <tuple>
#include <utility>

#include "graph.h"
#include "hlc.h"
#include "library.h"
#include "spacereplay.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

int &activeScopes() noexcept {
    thread_local int scopes = 0;
    return scopes;
}

[[nodiscard]] QString compactJson(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString formatsJson(const Library &library, qint64 bookId) {
    QJsonArray array;
    for (QSqlQuery query =
             ChangeLog::query(library, "SELECT format, file, size, hash FROM formats WHERE book_id = ? ORDER BY format", {bookId});
         query.next();) {
        array << QJsonObject{{"format", query.value(0).toString()},
                             {"file", query.value(1).toString()},
                             {"size", query.value(2).toLongLong()},
                             {"hash", query.value(3).toString()}};
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString progressJson(const Progress &progress) {
    return compactJson({{"section", progress.section}, {"fraction", progress.fraction}, {"percent", progress.percent}});
}

struct LoggedValue {
    qint64 hlc = -1;
    QString device;
    QString value;
};

using FieldMap = QHash<QString, LoggedValue>;

struct EntityContext {
    const Library *library = nullptr;
    QString space;
    QString entity;
    QString gid;
    FieldMap existing;
    bool baseline = false;
};

[[nodiscard]] FieldMap loggedFields(const Library &library, const QString &entity, const QString &globalId) {
    FieldMap map;
    for (QSqlQuery query = ChangeLog::query(library, "SELECT field, value, hlc, device FROM ops WHERE entity = ? AND id = ? ORDER BY seq",
                                            {entity, globalId});
         query.next();) {
        const QString field = query.value(0).toString();
        const QString value = query.value(1).toString();
        const qint64 hlc = query.value(2).toLongLong();
        const QString device = query.value(3).toString();
        const auto it = map.constFind(field);
        if (it == map.cend() || std::tie(hlc, device, value) >= std::tie(it->hlc, it->device, it->value))
            map.insert(field, {hlc, device, value});
    }
    return map;
}

void writeField(const EntityContext &ctx, const QString &field, const QString &value, qint64 rowHlc) {
    const auto it = ctx.existing.constFind(field);
    if (it != ctx.existing.cend() && it->value == value)
        return;
    qint64 useHlc = rowHlc;
    if (!ctx.baseline && it != ctx.existing.cend() && useHlc > 0 && useHlc <= it->hlc)
        useHlc = Spaces::tickHlc(*ctx.library);
    ChangeLog::append(*ctx.library, {ctx.space, ctx.entity, ctx.gid, field, value, useHlc});
}

void clampBookDetailHlcsIfBackdated(const Library &library, const QString &gid, qint64 detailsAt, FieldMap &existing) {
    qint64 maxDetailHlc = 0;
    for (auto it = existing.cbegin(); it != existing.cend(); ++it) {
        const QString &key = it.key();
        if (key != "progress"_L1 && key != "shelf"_L1 && key != "rating"_L1 && key != "finished"_L1 && key != "paper"_L1
            && key != "removed"_L1)
            maxDetailHlc = std::max(maxDetailHlc, it.value().hlc);
    }
    if (maxDetailHlc > 0 && Hlc::seconds(detailsAt) < Hlc::seconds(maxDetailHlc)) {
        const qint64 target = Hlc::normalize(detailsAt);
        ChangeLog::exec(library,
                        "UPDATE ops SET hlc = ? WHERE entity = 'book' AND id = ? AND hlc > ? "
                        "AND field NOT IN ('progress', 'shelf', 'rating', 'finished', 'paper', 'removed')",
                        {target, gid, target});
        existing = loggedFields(library, u"book"_s, gid);
    }
}

void syncBookReading(const EntityContext &ctx, const Book &book) {
    const bool hasReading = !book.shelf.isEmpty() || book.rating > 0 || !book.finished.isEmpty() || book.paper || book.readingAt > 0
                            || ctx.existing.contains(u"shelf"_s);
    if (hasReading) {
        const qint64 readingHlc = Hlc::normalize(book.readingAt);
        writeField(ctx, u"shelf"_s, book.shelf, readingHlc);
        writeField(ctx, u"rating"_s, QString::number(book.rating), readingHlc);
        writeField(ctx, u"finished"_s, book.finished, readingHlc);
        writeField(ctx, u"paper"_s, book.paper ? u"1"_s : u"0"_s, readingHlc);
    }
    const Progress prog = ctx.library->progress(book.id);
    if (prog.updated > 0 || prog.percent > 0 || prog.section > 0 || prog.fraction > 0)
        writeField(ctx, u"progress"_s, progressJson(prog), Hlc::normalize(prog.updated));
    if (ctx.existing.contains(u"removed"_s) && ctx.existing.value(u"removed"_s).value != "0"_L1)
        ChangeLog::append(*ctx.library, {ctx.space, u"book"_s, ctx.gid, u"removed"_s, u"0"_s, Spaces::tickHlc(*ctx.library)});
}

void syncBookToLog(const Library &library, const Book &book, bool baseline) {
    const QString gid = Spaces::bookId(library, book.id);
    if (gid.isEmpty())
        return;
    FieldMap existing = loggedFields(library, u"book"_s, gid);
    if (!baseline)
        clampBookDetailHlcsIfBackdated(library, gid, book.detailsAt, existing);
    const EntityContext ctx{&library, Spaces::personalSpace(library), u"book"_s, gid, existing, baseline};
    const qint64 last = Spaces::lastHlc(library);
    const qint64 detailsHlc =
        (!baseline && book.detailsAt > 0 && Hlc::seconds(last) == book.detailsAt) ? last : Hlc::normalize(book.detailsAt);
    const QList<std::pair<QString, QString>> details{
        {u"title"_s, book.meta.title},
        {u"authors"_s, book.meta.authorText()},
        {u"series"_s, book.meta.series},
        {u"series_index"_s, QString::number(book.meta.seriesIndex, 'g', 12)},
        {u"publisher"_s, book.meta.publisher},
        {u"language"_s, book.meta.language},
        {u"isbn"_s, book.meta.isbn},
        {u"published"_s, book.meta.published},
        {u"description"_s, book.meta.description},
        {u"tags"_s, book.meta.tags.join(", "_L1)},
        {u"identifiers"_s, compactJson(book.identifiers)},
        {u"added"_s, QString::number(book.added)},
        {u"has_cover"_s, book.hasCover ? u"1"_s : u"0"_s},
        {u"path"_s, book.path},
        {u"formats"_s, formatsJson(library, book.id)},
    };
    for (const auto &[field, value] : details)
        writeField(ctx, field, value, detailsHlc);
    syncBookReading(ctx, book);
}

void syncCollectionToLog(const Library &library, const Collection &col, bool baseline) {
    const QString gid = Spaces::collectionId(library, col.id);
    if (gid.isEmpty())
        return;
    const EntityContext ctx{&library, Spaces::personalSpace(library), u"collection"_s, gid, loggedFields(library, u"collection"_s, gid),
                            baseline};
    const qint64 colHlc = Hlc::normalize(col.updated);
    const QString parentGid = col.parent > 0 ? Spaces::collectionId(library, col.parent) : QString();
    writeField(ctx, u"name"_s, col.name, colHlc);
    writeField(ctx, u"parent"_s, parentGid, colHlc);
    if (col.removed) {
        writeField(ctx, u"removed"_s, u"1"_s, colHlc);
        return;
    }
    writeField(ctx, u"removed"_s, u"0"_s, colHlc);
    writeField(ctx, u"rule"_s, col.rule, colHlc);
    writeField(ctx, u"shelf"_s, compactJson(col.shelf), colHlc);
    writeField(ctx, u"format"_s, compactJson(col.format), colHlc);

    QSet<QString> currentBooks;
    for (const qint64 bookId : col.included) {
        const QString field = u"book:"_s + Spaces::bookId(library, bookId);
        currentBooks.insert(field);
        writeField(ctx, field, u"in"_s, colHlc);
    }
    for (const qint64 bookId : col.excluded) {
        const QString field = u"book:"_s + Spaces::bookId(library, bookId);
        currentBooks.insert(field);
        writeField(ctx, field, u"out"_s, colHlc);
    }
    for (auto it = ctx.existing.cbegin(); it != ctx.existing.cend(); ++it) {
        if (it.key().startsWith("book:"_L1) && !it.value().value.isEmpty() && !currentBooks.contains(it.key()))
            writeField(ctx, it.key(), QString(), colHlc);
    }
}

void syncNodeRowToLog(const Library &library, const QSqlQuery &query, bool baseline) {
    const qint64 id = query.value(0).toLongLong();
    const QString scope = query.value(1).toString();
    const QString gid = Spaces::nodeId(library, id);
    const EntityContext ctx{&library, Spaces::spaceForScope(library, scope), u"node"_s,
                            gid,      loggedFields(library, u"node"_s, gid), baseline};
    const qint64 hlc = Hlc::normalize(query.value(7).toLongLong());
    const qint64 bookId = query.value(6).toLongLong();
    writeField(ctx, u"scope"_s, scope, hlc);
    writeField(ctx, u"kind"_s, query.value(2).toString(), hlc);
    writeField(ctx, u"name"_s, query.value(3).toString(), hlc);
    writeField(ctx, u"summary"_s, query.value(4).toString(), hlc);
    writeField(ctx, u"data"_s, query.value(5).toString(), hlc);
    writeField(ctx, u"book"_s, bookId > 0 ? Spaces::bookId(library, bookId) : QString(), hlc);
    if (ctx.existing.contains(u"removed"_s) && ctx.existing.value(u"removed"_s).value == "1"_L1)
        ChangeLog::append(library, {ctx.space, u"node"_s, gid, u"removed"_s, u"0"_s, Spaces::tickHlc(library)});
    for (QSqlQuery seen = ChangeLog::query(
             library, "SELECT book_id, section, fraction, anchor FROM graph_seen WHERE node_id = ? ORDER BY book_id", {id});
         seen.next();) {
        const QString bookGid = Spaces::bookId(library, seen.value(0).toLongLong());
        if (!bookGid.isEmpty())
            writeField(
                ctx, u"seen:"_s + bookGid,
                compactJson(
                    {{"section", seen.value(1).toInt()}, {"fraction", seen.value(2).toDouble()}, {"anchor", seen.value(3).toString()}}),
                hlc);
    }
}

void syncFactsToLog(const Library &library) {
    for (QSqlQuery query =
             ChangeLog::query(library, "SELECT f.id, f.node_id, f.kind, f.value, COALESCE(f.book_id, 0), f.section, f.fraction, f.anchor, "
                                       "n.scope, n.updated FROM graph_facts AS f JOIN graph_nodes AS n ON n.id = f.node_id ORDER BY f.id");
         query.next();) {
        const QString gid = Spaces::factId(library, query.value(0).toLongLong());
        const qint64 bookId = query.value(4).toLongLong();
        const QString space = Spaces::spaceForScope(library, query.value(8).toString());
        const EntityContext ctx{&library, space, u"fact"_s, gid, loggedFields(library, u"fact"_s, gid), true};
        writeField(ctx, u"revealed"_s,
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

void syncEdgesToLog(const Library &library) {
    for (QSqlQuery query =
             ChangeLog::query(library, "SELECT e.id, e.source, e.target, e.kind, e.note, e.book_id, e.section, e.fraction, e.anchor, "
                                       "n.scope, n.updated FROM graph_edges AS e JOIN graph_nodes AS n ON n.id = e.source ORDER BY e.id");
         query.next();) {
        const QString gid = Spaces::edgeId(library, query.value(0).toLongLong());
        const qint64 bookId = query.value(5).toLongLong();
        const QString space = Spaces::spaceForScope(library, query.value(9).toString());
        const EntityContext ctx{&library, space, u"edge"_s, gid, loggedFields(library, u"edge"_s, gid), true};
        writeField(ctx, u"link"_s,
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

void syncGraphToLog(const Library &library, bool baseline) {
    for (QSqlQuery query = ChangeLog::query(
             library, "SELECT id, scope, kind, name, summary, data, COALESCE(book_id, 0), updated FROM graph_nodes ORDER BY id");
         query.next();)
        syncNodeRowToLog(library, query, baseline);
    syncFactsToLog(library);
    syncEdgesToLog(library);
    for (QSqlQuery query =
             ChangeLog::query(library, "SELECT book_id, day, from_percent, to_percent FROM reading_log ORDER BY book_id, day");
         query.next();) {
        const QString bookGid = Spaces::bookId(library, query.value(0).toLongLong());
        if (bookGid.isEmpty())
            continue;
        const QString day = query.value(1).toString();
        const QString gid = bookGid + u":"_s + day;
        const EntityContext ctx{
            &library, Spaces::personalSpace(library), u"reading_log"_s, gid, loggedFields(library, u"reading_log"_s, gid), true};
        writeField(ctx, u"range"_s,
                   compactJson({{"book", bookGid}, {"day", day}, {"from", query.value(2).toDouble()}, {"to", query.value(3).toDouble()}}),
                   Spaces::lastHlc(library));
    }
}

void detectDeletedBooks(const Library &library) {
    QSet<QString> currentBooks;
    for (QSqlQuery query = ChangeLog::query(library, "SELECT id FROM books"); query.next();)
        currentBooks.insert(Spaces::bookId(library, query.value(0).toLongLong()));
    for (const Library::Removal &removal : library.removals())
        ChangeLog::recordRemoval(library, removal.key, removal.at);
    for (QSqlQuery query = ChangeLog::query(library, "SELECT DISTINCT id FROM ops WHERE entity = 'book'"); query.next();) {
        const QString gid = query.value(0).toString();
        if (currentBooks.contains(gid))
            continue;
        const FieldMap existing = loggedFields(library, u"book"_s, gid);
        if (!existing.contains(u"removed"_s) || existing.value(u"removed"_s).value == "0"_L1)
            ChangeLog::append(library,
                              {Spaces::personalSpace(library), u"book"_s, gid, u"removed"_s, gid.mid(2), Spaces::tickHlc(library)});
    }
}

void detectDeletedGraphEntity(const Library &library, const QString &entity, const QString &table) {
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
    for (QSqlQuery query = ChangeLog::query(library, "SELECT DISTINCT space, id FROM ops WHERE entity = ?", {entity}); query.next();) {
        const QString space = query.value(0).toString();
        const QString gid = query.value(1).toString();
        if (alive.contains(gid))
            continue;
        const FieldMap existing = loggedFields(library, entity, gid);
        if (!existing.contains(u"removed"_s) || existing.value(u"removed"_s).value != "1"_L1)
            ChangeLog::append(library, {space, entity, gid, u"removed"_s, u"1"_s, Spaces::tickHlc(library)});
    }
}

[[nodiscard]] bool countersMatch(const Library &library) {
    const Library::Changes now = library.changes();
    QHash<QString, qint64> logged;
    for (QSqlQuery query = ChangeLog::query(
             library, "SELECT key, value FROM library_state WHERE key IN ('logged_books', 'logged_graph', 'logged_format')");
         query.next();)
        logged.insert(query.value(0).toString(), query.value(1).toLongLong());
    return logged.value(u"logged_books"_s, -1) == now.books && logged.value(u"logged_graph"_s, -1) == now.graph
           && logged.value(u"logged_format"_s, -1) == now.format;
}

} // namespace

ChangeLog::Scope::Scope() noexcept { ++activeScopes(); }

ChangeLog::Scope::~Scope() { --activeScopes(); }

void ChangeLog::open(const Library &library) {
    const Scope guard;
    QSqlQuery hasOps = ChangeLog::query(library, "SELECT 1 FROM ops LIMIT 1");
    const bool emptyOps = !hasOps.next();
    hasOps.finish();
    QSqlQuery logged = ChangeLog::query(library, "SELECT value FROM library_state WHERE key = 'logged_books'");
    const bool neverLogged = !logged.next() || logged.value(0).toLongLong() < 0;
    logged.finish();
    if (emptyOps && neverLogged)
        writeBaseline(library);
    else
        reconcile(library);
}

void ChangeLog::writeBaseline(const Library &library) {
    const Scope guard;
    const Library::Batch batch(library);
    Spaces::assignAll(library);
    for (const Book &book : library.books())
        syncBookToLog(library, book, true);
    for (const Library::Removal &removal : library.removals())
        recordRemoval(library, removal.key, removal.at);
    for (const Collection &collection : library.collections(true))
        syncCollectionToLog(library, collection, true);
    syncGraphToLog(library, true);
    markLogged(library);
}

void ChangeLog::reconcile(const Library &library) {
    const Scope guard;
    if (countersMatch(library))
        return;
    const Library::Batch batch(library);
    Spaces::assignAll(library);
    for (const Book &book : library.books())
        syncBookToLog(library, book, false);
    for (const Collection &collection : library.collections(true))
        syncCollectionToLog(library, collection, false);
    syncGraphToLog(library, false);
    detectDeletedBooks(library);
    detectDeletedGraphEntity(library, u"node"_s, u"graph_nodes"_s);
    detectDeletedGraphEntity(library, u"edge"_s, u"graph_edges"_s);
    detectDeletedGraphEntity(library, u"fact"_s, u"graph_facts"_s);
    markLogged(library);
}

void ChangeLog::afterWrite(const Library &library) {
    if (activeScopes() > 0)
        return;
    const Scope guard;
    QSqlQuery logged = ChangeLog::query(library, "SELECT value FROM library_state WHERE key = 'logged_books'");
    if (!logged.next() || logged.value(0).toLongLong() < 0)
        return;
    logged.finish();
    reconcile(library);
}

void ChangeLog::append(const Library &library, const Entry &entry) {
    const Scope guard;
    if (entry.id.isEmpty())
        return;
    const QString targetSpace = entry.space.isEmpty() ? Spaces::personalSpace(library) : entry.space;
    const QString device = Spaces::deviceId(library);
    const qint64 stamp = entry.hlc ? Hlc::normalize(*entry.hlc) : Spaces::tickHlc(library);
    if (entry.hlc && *entry.hlc > 0)
        Spaces::observeHlc(library, stamp);
    QSqlQuery nextSeq =
        ChangeLog::query(library, "SELECT COALESCE(MAX(seq), 0) + 1 FROM ops WHERE space = ? AND device = ?", {targetSpace, device});
    const qint64 seq = nextSeq.next() ? nextSeq.value(0).toLongLong() : 1;
    nextSeq.finish();
    ChangeLog::exec(library,
                    "INSERT INTO ops (space, device, seq, hlc, author, entity, id, field, value) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    {targetSpace, device, seq, stamp, device, entry.entity, entry.id, entry.field, entry.value});
    ChangeLog::exec(library,
                    "INSERT INTO seen (space, device, seq, hlc) VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(space, device) DO UPDATE SET seq = MAX(seq, excluded.seq), "
                    "hlc = MAX(hlc, excluded.hlc)",
                    {targetSpace, device, seq, stamp});
}

void ChangeLog::markLogged(const Library &library) {
    const Scope guard;
    const Library::Changes now = library.changes();
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('logged_books', ?)", {now.books});
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('logged_graph', ?)", {now.graph});
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('logged_format', ?)", {now.format});
}

void ChangeLog::setProgress(Library &library, qint64 bookId, const Progress &progress) {
    const Scope guard;
    const Library::Batch batch(library);
    const qint64 hlc = progress.updated > 0 ? Hlc::normalize(progress.updated) : Spaces::tickHlc(library);
    if (progress.updated > 0)
        Spaces::observeHlc(library, hlc);
    ChangeLog::exec(library, "INSERT OR REPLACE INTO progress (book_id, section, fraction, percent, updated) VALUES (?, ?, ?, ?, ?)",
                    {bookId, progress.section, progress.fraction, progress.percent, Hlc::seconds(hlc)});
    const QString gid = Spaces::bookId(library, bookId);
    if (!gid.isEmpty()) {
        if (progress.updated > 0) {
            ChangeLog::exec(library, "UPDATE ops SET hlc = ? WHERE entity = 'book' AND id = ? AND field = 'progress' AND hlc > ?",
                            {hlc, gid, hlc});
        }
        append(library, {Spaces::personalSpace(library), u"book"_s, gid, u"progress"_s, progressJson(progress), hlc});
    }
    markLogged(library);
}

bool ChangeLog::setDetailsAt(Library &library, qint64 bookId, qint64 at) {
    const Scope guard;
    const Library::Batch batch(library);
    const qint64 hlc = Hlc::normalize(at);
    if (hlc > 0)
        Spaces::observeHlc(library, hlc);
    if (!ChangeLog::exec(library, "UPDATE books SET details_at = ? WHERE id = ?", {Hlc::seconds(hlc), bookId}))
        return false;
    const QString gid = Spaces::bookId(library, bookId);
    if (!gid.isEmpty()) {
        ChangeLog::exec(library,
                        "UPDATE ops SET hlc = ? WHERE entity = 'book' AND id = ? "
                        "AND field NOT IN ('progress', 'shelf', 'rating', 'finished', 'paper', 'removed')",
                        {hlc, gid});
    }
    markLogged(library);
    return true;
}

void ChangeLog::recordRemoval(const Library &library, const QString &key, qint64 removedAt) {
    const Scope guard;
    const Library::Batch batch(library);
    const QString gid = Spaces::removalGlobalId(key);
    FieldMap existing = loggedFields(library, u"book"_s, gid);
    const qint64 hlc = Hlc::normalize(removedAt);
    if (existing.contains(u"removed"_s) && Hlc::seconds(removedAt) < Hlc::seconds(existing.value(u"removed"_s).hlc)) {
        ChangeLog::exec(library, "UPDATE ops SET hlc = ? WHERE entity = 'book' AND id = ? AND field = 'removed'", {hlc, gid});
        existing = loggedFields(library, u"book"_s, gid);
    }
    const EntityContext ctx{&library, Spaces::personalSpace(library), u"book"_s, gid, existing, true};
    writeField(ctx, u"removed"_s, key, hlc);
    markLogged(library);
}

QSqlQuery ChangeLog::query(const Library &library, const QString &sql, const QVariantList &values) { return library.query(sql, values); }

bool ChangeLog::exec(const Library &library, const QString &sql, const QVariantList &values) { return library.exec(sql, values); }

std::expected<qint64, QString> ChangeLog::insert(const Library &library, const QString &sql, const QVariantList &values) {
    return library.insert(sql, values);
}
