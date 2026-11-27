#include "changelogfields.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <cstdlib>
#include <tuple>

#include "changelog.h"
#include "hlc.h"
#include "library.h"
#include "spacereducer.h"
#include "spaces.h"
#include "statements.h"

using namespace Qt::StringLiterals;

ChangeLogFields::Map ChangeLogFields::logged(const Library &library, const QString &entity, const QString &globalId) {
    Map map;
    QHash<QString, SpaceReducer::Written> ownAt;
    const QString me = Spaces::deviceId(library);
    ChangeLog::statements(library).each(
        "SELECT o.field, o.value, o.hlc, d.name, s.name, o.seq FROM live_ops AS o JOIN log_ids AS d ON d.id = o.device "
        "JOIN log_ids AS s ON s.id = o.space WHERE o.entity = ? AND o.id = (SELECT id FROM log_ids WHERE name = ?) "
        "ORDER BY s.name, o.seq",
        {entity, globalId}, [&map, &ownAt, &me](const QSqlQuery &query) {
            const QString field = query.value(0).toString();
            const QString value = query.value(1).toString();
            const qint64 hlc = query.value(2).toLongLong();
            const QString device = query.value(3).toString();
            Logged &latest = map[field];
            if (std::tie(hlc, device, value) >= std::tie(latest.hlc, latest.device, latest.value))
                std::tie(latest.hlc, latest.device, latest.value) = std::tie(hlc, device, value);
            // What this device wrote last, as a sync reads it (SpaceReducer::reduce): of a device
            // that joined another's space, an operation there comes after any of its own space (#216).
            const SpaceReducer::Written at{query.value(4).toString(), query.value(5).toLongLong(), hlc};
            if (device != me)
                return;
            if (const auto before = ownAt.constFind(field); before == ownAt.cend() || SpaceReducer::writtenAfter(at, *before)) {
                ownAt.insert(field, at);
                latest.own = value;
                latest.ownHlc = hlc;
            }
        });
    return map;
}

namespace {

// The stamp of a value new on this device: the row's own time, unless that is
// a write just now or no later than what the log has of the field, which takes
// a new tick.
[[nodiscard]] qint64 newStamp(const ChangeLogFields::Context &context, const ChangeLogFields::Logged *logged, qint64 rowHlc) {
    // What an import read stays as old as that, whatever another computer said of the field (#284): each such
    // value of this device's a tick after its last. Unless this device said something newer of it before (a
    // book changed, removed and added again), which what it holds now has to come after.
    if (rowHlc > 0 && Hlc::seconds(rowHlc) == Hlc::asImported && (!logged || Hlc::seconds(logged->ownHlc) <= Hlc::asImported))
        return !logged || logged->ownHlc < rowHlc ? rowHlc : logged->ownHlc + 1;
    const bool isRecentWall = !context.baseline && rowHlc > 0 && Hlc::counter(rowHlc) == 0
                              && std::abs(Spaces::wallSeconds(*context.library) - Hlc::seconds(rowHlc)) <= 5;
    return isRecentWall || (logged && rowHlc <= logged->hlc) ? Spaces::tickHlc(*context.library) : rowHlc;
}

void writeOwn(ChangeLogFields::Context &context, const QString &field, const QString &value, qint64 hlc) {
    ChangeLog::append(*context.library, {context.space, context.entity, context.gid, field, value, hlc});
    context.existing.insert(field, {hlc, Spaces::deviceId(*context.library), value, value, hlc});
}

[[nodiscard]] QString compactJson(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString formatsJson(const Library &library, qint64 bookId) {
    QJsonArray array;
    ChangeLog::statements(library).each("SELECT format, file, size, hash FROM formats WHERE book_id = ? ORDER BY format", {bookId},
                                        [&array](const QSqlQuery &query) {
                                            array << QJsonObject{{"format", query.value(0).toString()},
                                                                 {"file", query.value(1).toString()},
                                                                 {"size", query.value(2).toLongLong()},
                                                                 {"hash", query.value(3).toString()}};
                                        });
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace

void ChangeLogFields::write(Context &context, const QString &field, const QString &value, qint64 rowHlc) {
    const auto it = context.existing.constFind(field);
    const Logged *logged = it != context.existing.cend() ? &*it : nullptr;
    // Of a book, what another device wrote is written by this one too, the
    // first time its row holds it: a sync reads what a computer holds of a
    // book from its device's operations alone, and without them the book
    // there had no title, description or ISBN, or wasn't there (#210). Of
    // the rest (the graph, collections, bookmarks), nothing reads it so, and
    // what was taken is not written again.
    const bool echoes = context.entity == "book"_L1;
    if (logged && (logged->own == value || (!echoes && !logged->own && logged->value == value)))
        return;
    // A value this device took from another's operation keeps that one's
    // stamp: an echo of it stamped now could beat a newer edit made there
    // meanwhile. At the same stamp and value it changes no outcome.
    writeOwn(context, field, value, logged && logged->value == value ? logged->hlc : newStamp(context, logged, rowHlc));
}

bool ChangeLogFields::echo(Context &context, const QString &field, const QString &value) {
    const auto logged = context.existing.constFind(field);
    if (logged == context.existing.cend() || logged->value != value || logged->own == value)
        return false;
    writeOwn(context, field, value, logged->hlc);
    return true;
}

ChangeLogFields::Fields ChangeLogFields::bookDetails(const Library &library, const Book &book) {
    const QString coverPath = library.coverPath(book.id);
    return {{u"title"_s, book.meta.title},
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
            {u"cover_hash"_s, coverPath.isEmpty() ? QString() : Library::hashFile(coverPath)},
            {u"path"_s, book.path},
            {u"formats"_s, formatsJson(library, book.id)}};
}

ChangeLogFields::Fields ChangeLogFields::bookReading(const Book &book) {
    return {{u"shelf"_s, book.shelf},
            {u"rating"_s, QString::number(book.rating)},
            {u"finished"_s, book.finished},
            {u"paper"_s, book.paper ? u"1"_s : u"0"_s}};
}

qsizetype ChangeLogFields::echoTaken(const Library &library) {
    const QString space = Spaces::personalSpace(library);
    qsizetype written = 0;
    for (const Book &book : library.books()) {
        const QString gid = Spaces::bookId(library, book.id);
        Context context{&library, space, u"book"_s, gid, logged(library, u"book"_s, gid), false};
        // A book this device wrote all of, as most are, is passed by without
        // reading its files (the cover's hash).
        if (gid.isEmpty() || std::ranges::all_of(context.existing, [](const Logged &field) { return field.own == field.value; }))
            continue;
        bool echoed = false;
        for (const Fields &fields : {bookDetails(library, book), bookReading(book)})
            for (const auto &[field, value] : fields)
                echoed = echo(context, field, value) || echoed;
        written += echoed ? 1 : 0;
    }
    return written;
}
