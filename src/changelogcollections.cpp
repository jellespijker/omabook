#include "changelogcollections.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlQuery>

#include "changelog.h"
#include "changelogfields.h"
#include "hlc.h"
#include "library.h"
#include "spacereplay.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

using EntityContext = ChangeLogFields::Context;

[[nodiscard]] QString compactJson(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// The book of this global id has a row in this library.
[[nodiscard]] bool bookIsHere(const Library &library, const QString &gid) {
    const qint64 id = Spaces::localId(library, u"book"_s, gid);
    if (id <= 0)
        return false;
    QSqlQuery row = ChangeLog::query(library, "SELECT 1 FROM books WHERE id = ?", {id});
    return row.next();
}

// A book in or out of a collection by hand. Held here while the log's latest
// word on it is another computer's that says otherwise: put back here after
// it was taken out there, which this device's own last word (the same) would
// otherwise hide, and the next sync would take it out again (#300).
void writeMembership(EntityContext &ctx, const QString &field, const QString &value, qint64 colHlc) {
    const auto logged = ctx.existing.constFind(field);
    if (logged == ctx.existing.cend() || logged->value == value || logged->own != value) {
        ChangeLogFields::write(ctx, field, value, colHlc);
        return;
    }
    const Library &library = *ctx.library;
    const qint64 hlc = Spaces::tickHlc(library);
    ChangeLog::append(library, {ctx.space, u"collection"_s, ctx.gid, field, value, hlc});
    ctx.existing.insert(field, {hlc, Spaces::deviceId(library), value, value});
}

// A collection's books by hand, each in or out, and the ones it no longer has.
void syncCollectionBooks(EntityContext &ctx, const Collection &col, qint64 colHlc) {
    const Library &library = *ctx.library;
    QSet<QString> currentBooks;
    for (const bool in : {true, false}) {
        for (const qint64 bookId : in ? col.included : col.excluded) {
            const QString field = u"book:"_s + Spaces::bookId(library, bookId);
            currentBooks.insert(field);
            writeMembership(ctx, field, in ? u"in"_s : u"out"_s, colHlc);
        }
    }
    // A book the log has in it that has no row here was taken out here only if
    // the book is here: one whose file hasn't come yet (a hub is sent a
    // library's collections before its books) can't have a row, and saying it
    // left would take it out of the collection everywhere (#280).
    for (auto it = ctx.existing.cbegin(); it != ctx.existing.cend(); ++it) {
        if (it.key().startsWith("book:"_L1) && !it.value().value.isEmpty() && !currentBooks.contains(it.key())
            && bookIsHere(library, it.key().mid(5)))
            ChangeLogFields::write(ctx, it.key(), QString(), colHlc);
    }
}

} // namespace

void ChangeLogCollections::syncToLog(const Library &library, const Collection &col, bool baseline, std::optional<qint64> explicitHlc) {
    const QString gid = Spaces::collectionId(library, col.id);
    if (gid.isEmpty())
        return;
    EntityContext ctx{
        &library, Spaces::personalSpace(library), u"collection"_s, gid, ChangeLogFields::logged(library, u"collection"_s, gid), baseline};
    const qint64 colHlc = explicitHlc ? Hlc::normalize(*explicitHlc) : Hlc::normalize(col.updated);
    const QString parentGid = col.parent > 0 ? Spaces::collectionId(library, col.parent) : QString();
    ChangeLogFields::write(ctx, u"name"_s, col.name, colHlc);
    ChangeLogFields::write(ctx, u"parent"_s, parentGid, colHlc);
    if (col.removed) {
        ChangeLogFields::write(ctx, u"removed"_s, u"1"_s, colHlc);
        return;
    }
    ChangeLogFields::write(ctx, u"removed"_s, u"0"_s, colHlc);
    ChangeLogFields::write(ctx, u"rule"_s, col.rule, colHlc);
    ChangeLogFields::write(ctx, u"shelf"_s, compactJson(col.shelf), colHlc);
    ChangeLogFields::write(ctx, u"format"_s, compactJson(col.format), colHlc);

    syncCollectionBooks(ctx, col, colHlc);
    if (explicitHlc && SpaceReplay::entityHlc(library, u"collection"_s, gid) < colHlc)
        ChangeLog::append(library, {ctx.space, u"collection"_s, gid, u"name"_s, col.name, colHlc});
}

void ChangeLogCollections::relogPutBack(const Library &library) {
    QSqlQuery done = ChangeLog::query(library, "SELECT 1 FROM library_state WHERE key = 'memberships_relogged'");
    if (done.next())
        return;
    done.finish();
    const Library::Batch batch(library);
    for (const Collection &collection : library.collections())
        syncToLog(library, collection, false);
    markPutBackLogged(library);
}

void ChangeLogCollections::markPutBackLogged(const Library &library) {
    ChangeLog::exec(library, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('memberships_relogged', 1)");
}
