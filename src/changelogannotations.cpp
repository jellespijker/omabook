#include "changelogannotations.h"

#include <QLocale>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <optional>
#include <utility>

#include "changelog.h"
#include "changelogfields.h"
#include "hlc.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto entity = "annotation"_L1;

[[nodiscard]] bool isRemoved(const ChangeLogFields::Map &existing) {
    const auto removed = existing.constFind(u"removed"_s);
    return removed != existing.cend() && removed->value == "1"_L1;
}

// One row of `SELECT id, book_id, kind, section, fraction, text, note, created`.
void syncRowToLog(const Library &library, const QSqlQuery &row, bool baseline) {
    const QString gid = Spaces::annotationId(library, row.value(0).toLongLong());
    const QString bookGid = Spaces::bookId(library, row.value(1).toLongLong());
    if (gid.isEmpty() || bookGid.isEmpty())
        return;
    ChangeLogFields::Context ctx{
        &library, Spaces::personalSpace(library), QString(entity), gid, ChangeLogFields::logged(library, QString(entity), gid), baseline};
    // Its own time is when it was made; a change since takes a new one (ChangeLogFields::write).
    const qint64 hlc = Hlc::normalize(row.value(7).toLongLong());
    const QList<std::pair<QString, QString>> fields{
        {u"book"_s, bookGid},
        {u"kind"_s, row.value(2).toString()},
        {u"section"_s, QString::number(row.value(3).toInt())},
        {u"fraction"_s, QString::number(row.value(4).toDouble(), 'g', QLocale::FloatingPointShortest)},
        {u"text"_s, row.value(5).toString()},
        {u"note"_s, row.value(6).toString()},
        {u"created"_s, QString::number(row.value(7).toLongLong())}};
    for (const auto &[field, value] : fields)
        ChangeLogFields::write(ctx, field, value, hlc);
    if (isRemoved(ctx.existing))
        ChangeLog::append(library, {ctx.space, QString(entity), gid, u"removed"_s, u"0"_s, Spaces::tickHlc(library)});
}

// " WHERE id IN (1, 2, 3)" for these ids, or nothing for all of them. The ids
// are numbers from the database, so they go into the statement as they are.
[[nodiscard]] QString onlyThese(const std::optional<QList<qint64>> &ids) {
    if (!ids)
        return {};
    QStringList listed;
    for (const qint64 id : *ids)
        listed << QString::number(id);
    return u" WHERE id IN ("_s + (listed.isEmpty() ? u"NULL"_s : listed.join(u',')) + u")"_s;
}

void syncRows(const Library &library, bool baseline, const std::optional<QList<qint64>> &ids) {
    for (QSqlQuery row = ChangeLog::query(library, u"SELECT id, book_id, kind, section, fraction, text, note, created FROM annotations"_s
                                                       + onlyThese(ids) + u" ORDER BY id"_s);
         row.next();)
        syncRowToLog(library, row, baseline);
}

[[nodiscard]] bool exists(const Library &library, const QString &table, qint64 id) {
    QSqlQuery query = ChangeLog::query(library, u"SELECT 1 FROM "_s + table + u" WHERE id = ?"_s, {id});
    return query.next();
}

// One that is gone from here: its tombstone, unless it has one, it never
// reached the log, or its book is gone too (removed here, which is the
// book's own tombstone; another computer that kept the book keeps it).
void tombstone(const Library &library, const QString &gid) {
    if (gid.isEmpty())
        return;
    const ChangeLogFields::Map existing = ChangeLogFields::logged(library, QString(entity), gid);
    if (existing.isEmpty() || isRemoved(existing))
        return;
    const qint64 book = Spaces::localId(library, u"book"_s, existing.value(u"book"_s).value);
    if (book <= 0 || !exists(library, u"books"_s, book))
        return;
    ChangeLog::append(library, {Spaces::personalSpace(library), QString(entity), gid, u"removed"_s, u"1"_s, Spaces::tickHlc(library)});
}

[[nodiscard]] qint64 state(const Library &library, const QString &key) {
    QSqlQuery query = ChangeLog::query(library, "SELECT value FROM library_state WHERE key = ?", {key});
    return query.next() ? query.value(0).toLongLong() : -1;
}

} // namespace

void ChangeLogAnnotations::syncToLog(const Library &library, bool baseline) { syncRows(library, baseline, std::nullopt); }

void ChangeLogAnnotations::syncRowsToLog(const Library &library, const QList<qint64> &ids) {
    if (ids.isEmpty())
        return;
    syncRows(library, false, ids);
    for (const qint64 id : ids)
        if (!exists(library, u"annotations"_s, id))
            tombstone(library, Spaces::globalId(library, QString(entity), id));
}

void ChangeLogAnnotations::detectDeleted(const Library &library) {
    QStringList gone;
    for (QSqlQuery query = ChangeLog::query(
             library, "SELECT global_id FROM global_ids WHERE entity = 'annotation' AND local_id NOT IN (SELECT id FROM annotations)");
         query.next();)
        gone << query.value(0).toString();
    for (const QString &gid : std::as_const(gone))
        tombstone(library, gid);
}

bool ChangeLogAnnotations::counted(const Library &library) {
    return state(library, u"logged_annotations"_s) == state(library, u"annotations"_s);
}

void ChangeLogAnnotations::markLogged(const Library &library) {
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('logged_annotations', ?)",
                    {state(library, u"annotations"_s)});
}
