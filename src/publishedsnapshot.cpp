// The snapshot of a space published here (PublishedSpace::snapshot and
// snapshotOf): its log as it is, for the file the owner gives. Kept apart
// from what chooses the space's operations, which it only follows.

#include "publishedspace.h"

#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <optional>
#include <utility>

#include "changelog.h"
#include "library.h"
#include "publishedlog.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// The snapshot of a space published here, of its log as it is: each
// operation with its number there, so that one who holds it is sent only
// what the log takes after it, and of a thing gone only its removal.
std::expected<SpaceSnapshot, QString> taken(const Library &library, const QString &space) {
    const std::optional<PublishedLog::Record> record = PublishedLog::recorded(library, space);
    QSqlQuery about = ChangeLog::query(library,
                                       "SELECT s.name, s.owner, m.name FROM spaces AS s LEFT JOIN space_members AS m "
                                       "ON m.space = s.id AND m.person = s.owner WHERE s.id = ?",
                                       {space});
    if (!record || !about.next())
        return std::unexpected(u"%1 is not a space published from this library."_s.arg(space));
    const qint64 collectionId = Spaces::localId(library, u"collection"_s, record->collection);
    const QList<Collection> collections = library.collections();
    const auto collection = std::ranges::find(collections, collectionId, &Collection::id);
    QList<SpaceOperation> ops = PublishedLog::compacted(PublishedLog::operations(library, space));
    qint64 clock = Spaces::lastHlc(library);
    for (const SpaceOperation &op : std::as_const(ops))
        clock = std::max(clock, op.hlc);
    return SpaceSnapshot{.space = space,
                         .kind = QString::fromLatin1(PublishedSpace::kind),
                         .name = collection != collections.end() ? collection->name : about.value(0).toString(),
                         .owner = {.name = about.value(2).toString(), .person = about.value(1).toString()},
                         .parts = record->parts,
                         .clock = clock,
                         .writtenBy = QString::fromLatin1(OMABOOK_VERSION),
                         .operations = std::move(ops)};
}

} // namespace

std::expected<SpaceSnapshot, QString> PublishedSpace::snapshot(const Library &library, qint64 collectionId, const Parts &parts,
                                                               const SpaceSnapshot::Owner &owner) {
    const auto published = publish(library, collectionId, parts, owner);
    if (!published)
        return std::unexpected(published.error());
    return taken(library, published->space);
}

std::expected<SpaceSnapshot, QString> PublishedSpace::snapshotOf(const Library &library, const QString &space) {
    const auto followed = follow(library, space);
    if (!followed)
        return std::unexpected(followed.error());
    return taken(library, space);
}
