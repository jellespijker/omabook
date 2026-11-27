#include "changelogremovals.h"

#include <QSqlQuery>
#include <QStringList>
#include <utility>

#include "changelog.h"
#include "changelogfields.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// A book this computer had rows of and has no more, found on opening: its
// tombstone, unless this computer already said it removed it (a word since
// that it is back is another computer's, and the next sync brings it here).
void logLostBook(const Library &library, const QString &gid) {
    const ChangeLogFields::Map existing = ChangeLogFields::logged(library, u"book"_s, gid);
    const auto removed = existing.constFind(u"removed"_s);
    const bool saidHere = removed != existing.cend() && removed->own && !removed->own->isEmpty() && *removed->own != "0"_L1;
    if (!saidHere)
        ChangeLogRemovals::gone(library, gid);
}

} // namespace

// A book that is gone: its tombstone, unless it already has one.
void ChangeLogRemovals::gone(const Library &library, const QString &gid) {
    if (gid.isEmpty())
        return;
    const ChangeLogFields::Map existing = ChangeLogFields::logged(library, u"book"_s, gid);
    if (!existing.isEmpty() && (!existing.contains(u"removed"_s) || existing.value(u"removed"_s).value == "0"_L1))
        ChangeLog::append(library, {Spaces::personalSpace(library), u"book"_s, gid, u"removed"_s, gid.mid(2), Spaces::tickHlc(library)});
}

void ChangeLogRemovals::detect(const Library &library) {
    for (const Library::Removal &removal : library.removals())
        ChangeLog::recordRemoval(library, removal.key, removal.at, false);
    // Only the books whose rows this computer had (it gave them their ids,
    // which stay when a row goes), never every book the log knows: one the
    // log knows only from another computer (its file never came, or it came
    // after the copy of this library that was put back) was never removed
    // here, and the next sync brings it (#61).
    QStringList lost;
    for (QSqlQuery query =
             ChangeLog::query(library, "SELECT global_id FROM global_ids WHERE entity = 'book' AND local_id NOT IN (SELECT id FROM books)");
         query.next();)
        lost << query.value(0).toString();
    for (const QString &gid : std::as_const(lost))
        logLostBook(library, gid);
}
