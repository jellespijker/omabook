#include "changelog.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <utility>

#include "changelogannotations.h"
#include "changelogbaseline.h"
#include "changelogcollections.h"
#include "changelogfields.h"
#include "changeloggraph.h"
#include "changelogremovals.h"
#include "graph.h"
#include "hlc.h"
#include "library.h"
#include "signedops.h"
#include "spacereducer.h"
#include "spacereplay.h"
#include "spaces.h"
#include "statements.h"
#include "tenancy.h"

using namespace Qt::StringLiterals;

namespace {

int &activeScopes() noexcept {
    thread_local int scopes = 0;
    return scopes;
}

[[nodiscard]] QString compactJson(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString progressJson(const Progress &progress) {
    return compactJson({{"section", progress.place.section}, {"fraction", progress.place.fraction}, {"percent", progress.percent}});
}

using FieldMap = ChangeLogFields::Map;
using EntityContext = ChangeLogFields::Context;

[[nodiscard]] ReducedBook reducedFrom(const FieldMap &existing) {
    ReducedBook book;
    for (auto it = existing.cbegin(); it != existing.cend(); ++it)
        SpaceReducer::foldBook(book, {{}, it.value().device, 0, it.value().hlc, {}, u"book"_s, {}, it.key(), it.value().value});
    return book;
}

void stampAtIfNeeded(EntityContext &ctx, const QString &stampField, qint64 targetHlc, bool mismatch) {
    if (ctx.baseline || !mismatch)
        return;
    const qint64 stamp = Spaces::tickHlc(*ctx.library);
    ChangeLog::append(*ctx.library, {ctx.space, u"book"_s, ctx.gid, stampField, QString::number(targetHlc), stamp});
    ctx.existing.insert(stampField, {stamp, Spaces::deviceId(*ctx.library), QString::number(targetHlc), QString::number(targetHlc)});
}

[[nodiscard]] qint64 targetProgressHlc(const EntityContext &ctx, const Progress &prog) {
    if (prog.updated > 0)
        return Hlc::normalize(prog.updated);
    return ctx.baseline ? 0 : Spaces::tickHlc(*ctx.library);
}

[[nodiscard]] bool progressUnchanged(const EntityContext &ctx, const QString &plainJson, qint64 targetHlc, bool explicitHlc) {
    const auto it = ctx.existing.constFind(u"progress"_s);
    if (it == ctx.existing.cend())
        return false;
    const QJsonObject oldObj = QJsonDocument::fromJson(it->value.toUtf8()).object();
    const QString oldPlain =
        compactJson({{"section", oldObj.value("section")}, {"fraction", oldObj.value("fraction")}, {"percent", oldObj.value("percent")}});
    const qint64 oldEffHlc = SpaceReducer::fieldHlc(reducedFrom(ctx.existing), u"progress"_s);
    const bool sameTime = explicitHlc ? (oldEffHlc == targetHlc) : (Hlc::seconds(oldEffHlc) == Hlc::seconds(targetHlc));
    return oldPlain == plainJson && sameTime;
}

void writeProgress(EntityContext &ctx, const Progress &prog, bool explicitHlc) {
    const qint64 targetHlc = targetProgressHlc(ctx, prog);
    const QString plainJson = progressJson(prog);
    if (progressUnchanged(ctx, plainJson, targetHlc, explicitHlc))
        return;
    const auto it = ctx.existing.constFind(u"progress"_s);
    qint64 opHlc = targetHlc;
    if (!ctx.baseline) {
        const bool isRecentWall = !explicitHlc && prog.updated > 0 && Hlc::counter(targetHlc) == 0
                                  && std::abs(Spaces::wallSeconds(*ctx.library) - Hlc::seconds(targetHlc)) <= 5;
        if (isRecentWall || (it != ctx.existing.cend() && opHlc <= it->hlc))
            opHlc = Spaces::tickHlc(*ctx.library);
    }
    const bool embedTargetHlc = Hlc::seconds(opHlc) != Hlc::seconds(targetHlc) || (explicitHlc && opHlc != targetHlc);
    const QString value =
        embedTargetHlc
            ? compactJson(
                  {{"section", prog.place.section}, {"fraction", prog.place.fraction}, {"percent", prog.percent}, {"hlc", targetHlc}})
            : plainJson;
    ChangeLog::append(*ctx.library, {ctx.space, u"book"_s, ctx.gid, u"progress"_s, value, opHlc});
    ctx.existing.insert(u"progress"_s, {opHlc, Spaces::deviceId(*ctx.library), value, value});
}

[[nodiscard]] bool hasReadingFields(const EntityContext &ctx, const Book &book) {
    return !book.shelf.isEmpty() || book.rating > 0 || !book.finished.isEmpty() || book.paper || book.readingAt > 0
           || ctx.existing.contains(u"shelf"_s);
}

[[nodiscard]] bool hasProgressFields(const Progress &prog) {
    return prog.updated > 0 || prog.percent > 0 || prog.place.section > 0 || prog.place.fraction > 0;
}

// Whether the row's reading is from before the log's: a sync stores the
// operations it takes before it fills in the rows, so a row written meanwhile
// (its details taken, its file arrived) still has the reading from before. That
// is no change made here, and written it would be one, stamped now, after the
// reading it was taken from and any newer one made elsewhere since, once
// passed on (#243). A change made here is stamped no earlier than the log's
// latest (Spaces::wallSeconds).
[[nodiscard]] bool readingBehind(const EntityContext &ctx, const Book &book) {
    return Hlc::seconds(book.readingAt) < Hlc::seconds(SpaceReducer::readingHlc(reducedFrom(ctx.existing)));
}

void syncBookReading(EntityContext &ctx, const Book &book) {
    if (hasReadingFields(ctx, book) && !readingBehind(ctx, book)) {
        const qint64 readingHlc = Hlc::normalize(book.readingAt);
        for (const auto &[field, value] : ChangeLogFields::bookReading(book))
            ChangeLogFields::write(ctx, field, value, readingHlc);
        const qint64 loggedReadingHlc = SpaceReducer::readingHlc(reducedFrom(ctx.existing));
        stampAtIfNeeded(ctx, u"reading_at"_s, readingHlc, Hlc::seconds(loggedReadingHlc) != Hlc::seconds(book.readingAt));
    }
    const Progress prog = ctx.library->progress(book.id);
    if (hasProgressFields(prog))
        writeProgress(ctx, prog, false);
    // Held here: said by this device while it, or whoever spoke last, says it was removed (#290).
    const auto removed = ctx.existing.constFind(u"removed"_s);
    if (removed != ctx.existing.cend() && (removed->value != "0"_L1 || removed->own.value_or(u"0"_s) != "0"_L1))
        ChangeLog::append(*ctx.library, {ctx.space, u"book"_s, ctx.gid, u"removed"_s, u"0"_s, Spaces::tickHlc(*ctx.library)});
}

void syncBookToLog(const Library &library, const Book &book, bool baseline, std::optional<qint64> explicitDetailsHlc = std::nullopt) {
    const QString gid = Spaces::bookId(library, book.id);
    if (gid.isEmpty())
        return;
    EntityContext ctx{&library, Spaces::personalSpace(library), u"book"_s, gid, ChangeLogFields::logged(library, u"book"_s, gid), baseline};
    const qint64 detailsHlc = explicitDetailsHlc ? Hlc::normalize(*explicitDetailsHlc) : Hlc::normalize(book.detailsAt);
    for (const auto &[field, value] : ChangeLogFields::bookDetails(library, book))
        ChangeLogFields::write(ctx, field, value, detailsHlc);
    const qint64 loggedDetailsHlc = SpaceReducer::detailsHlc(reducedFrom(ctx.existing));
    // A row as an import read it (#284) stamps no field back to that: one another computer's word is newer
    // than, and the log's merged times with it, stays so.
    const bool detailsMismatch =
        explicitDetailsHlc ? (loggedDetailsHlc != detailsHlc)
                           : (book.detailsAt != Hlc::asImported && Hlc::seconds(loggedDetailsHlc) != Hlc::seconds(book.detailsAt));
    stampAtIfNeeded(ctx, u"details_at"_s, detailsHlc, detailsMismatch);
    syncBookReading(ctx, book);
}

// Takes the rows a write touched into the log, each as reconcile would.
void logRows(const Library &library, const QHash<QString, QList<qint64>> &dirty) {
    if (dirty.contains(u"removal"_s))
        for (const Library::Removal &removal : library.removals())
            ChangeLog::recordRemoval(library, removal.key, removal.at, false);
    for (const qint64 id : dirty.value(u"book"_s)) {
        if (const std::optional<Book> book = library.book(id))
            syncBookToLog(library, *book, false);
        else
            ChangeLogRemovals::gone(library, Spaces::bookId(library, id));
    }
    if (const QList<qint64> touched = dirty.value(u"collection"_s); !touched.isEmpty())
        for (const Collection &collection : library.collections(true))
            if (touched.contains(collection.id))
                ChangeLogCollections::syncToLog(library, collection, false);
    ChangeLogGraph::syncRowsToLog(library, {.nodes = dirty.value(u"node"_s),
                                            .facts = dirty.value(u"fact"_s),
                                            .edges = dirty.value(u"edge"_s),
                                            .readingBooks = dirty.value(u"reading_log"_s)});
    ChangeLogAnnotations::syncRowsToLog(library, dirty.value(u"annotation"_s));
}

[[nodiscard]] bool countersMatch(const Library &library) {
    const Library::Changes now = library.changes();
    QHash<QString, qint64> logged;
    for (QSqlQuery query = ChangeLog::query(
             library, "SELECT key, value FROM library_state WHERE key IN ('logged_books', 'logged_graph', 'logged_format')");
         query.next();)
        logged.insert(query.value(0).toString(), query.value(1).toLongLong());
    return logged.value(u"logged_books"_s, -1) == now.books && logged.value(u"logged_graph"_s, -1) == now.graph
           && logged.value(u"logged_format"_s, -1) == now.format && ChangeLogAnnotations::counted(library);
}

} // namespace

ChangeLog::Scope::Scope() noexcept { ++activeScopes(); }

ChangeLog::Scope::~Scope() { --activeScopes(); }

void ChangeLog::open(const Library &library) {
    const Scope guard;
    QSqlQuery hasOps = ChangeLog::query(library, "SELECT 1 FROM live_ops LIMIT 1");
    const bool emptyOps = !hasOps.next();
    hasOps.finish();
    QSqlQuery logged = ChangeLog::query(library, "SELECT value FROM library_state WHERE key = 'logged_books'");
    const bool neverLogged = !logged.next() || logged.value(0).toLongLong() < 0;
    logged.finish();
    if (emptyOps && neverLogged) {
        writeBaseline(library);
    } else {
        reconcile(library);
        ChangeLogCollections::relogPutBack(library);
    }
}

void ChangeLog::writeBaseline(const Library &library) {
    const Scope guard;
    const Library::Batch batch(library);
    Spaces::assignAll(library);
    // As a snapshot, the log starting after it (#54).
    ChangeLogBaseline baseline(library);
    for (const Book &book : library.books())
        syncBookToLog(library, book, true);
    for (const Library::Removal &removal : library.removals())
        recordRemoval(library, removal.key, removal.at, true);
    for (const Collection &collection : library.collections(true))
        ChangeLogCollections::syncToLog(library, collection, true);
    ChangeLogGraph::syncToLog(library, true);
    ChangeLogAnnotations::syncToLog(library, true);
    baseline.finish();
    ChangeLog::exec(library, "DELETE FROM log_dirty"); // all of it is in the log now
    markLogged(library);
    ChangeLogCollections::markPutBackLogged(library);
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
        ChangeLogCollections::syncToLog(library, collection, false);
    ChangeLogGraph::syncToLog(library, false);
    ChangeLogRemovals::detect(library);
    ChangeLogGraph::detectDeletedEntity(library, u"node"_s, u"graph_nodes"_s);
    ChangeLogGraph::detectDeletedEntity(library, u"edge"_s, u"graph_edges"_s);
    ChangeLogGraph::detectDeletedEntity(library, u"fact"_s, u"graph_facts"_s);
    ChangeLogAnnotations::syncToLog(library, false);
    ChangeLogAnnotations::detectDeleted(library);
    ChangeLog::exec(library, "DELETE FROM log_dirty"); // all of it is in the log now
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
    // Only the rows this write touched (the triggers of log_dirty), never the
    // whole library: that is for opening a library a write bypassed (reconcile).
    QHash<QString, QList<qint64>> dirty;
    for (QSqlQuery rows = ChangeLog::query(library, "SELECT entity, local_id FROM log_dirty"); rows.next();)
        dirty[rows.value(0).toString()] << rows.value(1).toLongLong();
    if (dirty.isEmpty())
        return;
    const Library::Batch batch(library);
    logRows(library, dirty);
    Tenancy::followWrite(library, dirty);
    ChangeLog::exec(library, "DELETE FROM log_dirty");
    markLogged(library);
}

qint64 ChangeLog::internId(const Library &library, const QString &name) {
    if (const auto found = statements(library).first("SELECT id FROM log_ids WHERE name = ?", {name}))
        return found->toLongLong();
    return ChangeLog::insert(library, "INSERT INTO log_ids (name) VALUES (?)", {name}).value_or(0);
}

void ChangeLog::append(const Library &library, const Entry &entry) { appendAs(library, entry, library.person()); }

void ChangeLog::append(const Library &library, const Entry &entry, const Person &signer) { appendAs(library, entry, &signer); }

void ChangeLog::appendAs(const Library &library, const Entry &entry, const Person *person) {
    const Scope guard;
    if (entry.id.isEmpty())
        return;
    if (ChangeLogBaseline *baseline = ChangeLogBaseline::writing()) {
        baseline->hold(entry);
        return;
    }
    const QString targetSpace = entry.space.isEmpty() ? Spaces::personalSpace(library) : entry.space;
    const QString device = Spaces::deviceId(library);
    const qint64 spaceNum = internId(library, targetSpace);
    const qint64 deviceNum = internId(library, device);
    const qint64 idNum = internId(library, entry.id);
    const qint64 stamp = entry.hlc ? Hlc::normalize(*entry.hlc) : Spaces::tickHlc(library);
    if (entry.hlc && *entry.hlc > 0)
        Spaces::observeHlc(library, stamp);
    // After the last this device wrote here, also of what compaction took out
    // of `ops` into the snapshot (its mark): a number is never given twice.
    const qint64 seq = statements(library)
                           .first("SELECT MAX(COALESCE((SELECT MAX(seq) FROM ops WHERE space = ? AND device = ?), 0), "
                                  "COALESCE((SELECT seq FROM snapshot_marks WHERE space = ? AND device = ?), 0)) + 1",
                                  {spaceNum, deviceNum, spaceNum, deviceNum})
                           .value_or(1)
                           .toLongLong();
    // Signed by the person, as it is kept: the very bytes stored are the ones
    // signed. Without a person it goes unsigned, its author this computer, as
    // every operation did before there were people.
    SpaceOperation op{targetSpace, device, seq, stamp, device, entry.entity, entry.id, entry.field, entry.value};
    QVariant signer;
    if (person != nullptr) {
        SignedOps::signAs(op, *person);
        signer = internId(library, op.signer);
    }
    ChangeLog::exec(library,
                    "INSERT INTO ops (space, device, seq, hlc, author, entity, id, field, value, signer, signature) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    {spaceNum, deviceNum, seq, stamp, signer.isNull() ? QVariant(deviceNum) : signer, entry.entity, idNum, entry.field,
                     entry.value, signer, op.signature.isEmpty() ? QVariant() : QVariant(op.signature)});
    ChangeLog::exec(library,
                    "INSERT INTO seen (space, device, seq, hlc) VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(space, device) DO UPDATE SET seq = MAX(seq, excluded.seq), "
                    "hlc = MAX(hlc, excluded.hlc)",
                    {spaceNum, deviceNum, seq, stamp});
}

void ChangeLog::markLogged(const Library &library) {
    const Scope guard;
    const Library::Changes now = library.changes();
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('logged_books', ?)", {now.books});
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('logged_graph', ?)", {now.graph});
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('logged_format', ?)", {now.format});
    ChangeLogAnnotations::markLogged(library);
}

void ChangeLog::syncCollectionAt(const Library &library, qint64 collectionId, qint64 hlc) {
    const Scope guard;
    const qint64 normalized = Hlc::normalize(hlc);
    if (normalized > 0)
        Spaces::observeHlc(library, normalized);
    for (const Collection &collection : library.collections(true)) {
        if (collection.id == collectionId) {
            ChangeLogCollections::syncToLog(library, collection, true, normalized);
            break;
        }
    }
    ChangeLog::exec(library, "DELETE FROM log_dirty WHERE entity = 'collection' AND local_id = ?", {collectionId});
}

bool ChangeLog::setProgress(Library &library, qint64 bookId, const Progress &progress) {
    afterWrite(library);
    const Scope guard;
    const Library::Batch batch(library);
    const qint64 hlc = progress.updated > 0 ? Hlc::normalize(progress.updated) : Spaces::tickHlc(library);
    if (progress.updated > 0)
        Spaces::observeHlc(library, hlc);
    const bool kept =
        ChangeLog::exec(library, "INSERT OR REPLACE INTO progress (book_id, section, fraction, percent, updated) VALUES (?, ?, ?, ?, ?)",
                        {bookId, progress.place.section, progress.place.fraction, progress.percent, Hlc::seconds(hlc)});
    const QString gid = Spaces::bookId(library, bookId);
    if (!gid.isEmpty()) {
        EntityContext ctx{&library, Spaces::personalSpace(library), u"book"_s, gid, ChangeLogFields::logged(library, u"book"_s, gid),
                          false};
        Progress stamped = progress;
        stamped.updated = hlc;
        writeProgress(ctx, stamped, true);
    }
    ChangeLog::exec(library, "DELETE FROM log_dirty WHERE entity = 'book' AND local_id = ?", {bookId});
    markLogged(library);
    return kept;
}

bool ChangeLog::setDetailsAt(Library &library, qint64 bookId, qint64 at) {
    const Scope guard;
    const Library::Batch batch(library);
    const qint64 hlc = Hlc::normalize(at);
    if (hlc > 0)
        Spaces::observeHlc(library, hlc);
    if (!ChangeLog::exec(library, "UPDATE books SET details_at = ? WHERE id = ?", {Hlc::seconds(hlc), bookId}))
        return false;
    if (const std::optional<Book> book = library.book(bookId))
        syncBookToLog(library, *book, false, hlc);
    ChangeLog::exec(library, "DELETE FROM log_dirty WHERE entity = 'book' AND local_id = ?", {bookId});
    markLogged(library);
    return true;
}

void ChangeLog::recordRemoval(const Library &library, const QString &key, qint64 removedAt, bool baseline) {
    if (!baseline)
        afterWrite(library);
    const Scope guard;
    const Library::Batch batch(library);
    const QString gid = Spaces::removalGlobalId(key);
    EntityContext ctx{&library, Spaces::personalSpace(library), u"book"_s, gid, ChangeLogFields::logged(library, u"book"_s, gid), baseline};
    const qint64 hlc = Hlc::normalize(removedAt);
    ChangeLogFields::write(ctx, u"removed"_s, key, hlc);
    const qint64 effHlc = SpaceReducer::fieldHlc(reducedFrom(ctx.existing), u"removed"_s);
    stampAtIfNeeded(ctx, u"removed_at"_s, hlc, Hlc::seconds(effHlc) != Hlc::seconds(removedAt));
    ChangeLog::exec(library, "DELETE FROM log_dirty WHERE entity = 'removal'");
    markLogged(library);
}

QSqlQuery ChangeLog::query(const Library &library, const QString &sql, const QVariantList &values) { return library.query(sql, values); }

bool ChangeLog::exec(const Library &library, const QString &sql, const QVariantList &values) { return library.exec(sql, values); }

Statements &ChangeLog::statements(const Library &library) { return *library.m_statements; }

std::expected<qint64, QString> ChangeLog::insert(const Library &library, const QString &sql, const QVariantList &values) {
    return library.insert(sql, values);
}
