#include "subscribedspace.h"

#include <QHash>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <optional>

#include "changelog.h"
#include "compaction.h"
#include "hlc.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "signedops.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// What makes two operations one: the same writer, clock, thing, field and
// value. Their numbers are not, as a snapshot numbers its operations anew.
[[nodiscard]] QString sameness(const QString &device, qint64 hlc, const QString &entity, const QString &id, const QString &field,
                               const QString &value) {
    return QStringList{device, QString::number(Hlc::normalize(hlc)), entity, id, field, value}.join(u'\x1f');
}

[[nodiscard]] QString sameness(const SpaceOperation &op) { return sameness(op.device, op.hlc, op.entity, op.id, op.field, op.value); }

// What the library holds of a space, in `ops` and in its snapshot, whatever
// compaction did with it, and each device's last number there.
struct Held {
    QSet<QString> operations;
    QHash<QString, qint64> last;
};

[[nodiscard]] Held held(const Library &library, const QString &space) {
    Held held;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT d.name, o.seq, o.hlc, o.entity, i.name, o.field, o.value FROM ("
                                            "SELECT space, device, seq, hlc, entity, id, field, value FROM ops UNION ALL "
                                            "SELECT space, device, seq, hlc, entity, id, field, value FROM snapshot_ops) AS o "
                                            "JOIN log_ids AS d ON d.id = o.device JOIN log_ids AS i ON i.id = o.id "
                                            "WHERE o.space = (SELECT id FROM log_ids WHERE name = ?)",
                                            {space});
         query.next();) {
        const QString device = query.value(0).toString();
        held.operations.insert(sameness(device, query.value(2).toLongLong(), query.value(3).toString(), query.value(4).toString(),
                                        query.value(5).toString(), query.value(6).toString()));
        held.last[device] = std::max(held.last.value(device), query.value(1).toLongLong());
    }
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT d.name, m.seq FROM snapshot_marks AS m JOIN log_ids AS d ON d.id = m.device "
                                            "WHERE m.space = (SELECT id FROM log_ids WHERE name = ?)",
                                            {space});
         query.next();)
        held.last[query.value(0).toString()] = std::max(held.last.value(query.value(0).toString()), query.value(1).toLongLong());
    return held;
}

[[nodiscard]] bool spaceKnown(const Library &library, const QString &space) {
    QSqlQuery query = ChangeLog::query(library, "SELECT 1 FROM spaces WHERE id = ?", {space});
    return query.next();
}

void keep(const Library &library, const SpaceOperation &op, qint64 seq) {
    const qint64 space = ChangeLog::internId(library, op.space);
    const qint64 device = ChangeLog::internId(library, op.device);
    const bool isSigned = !op.signer.isEmpty() && !op.signature.isEmpty();
    ChangeLog::exec(library,
                    "INSERT INTO ops (space, device, seq, hlc, author, entity, id, field, value, signer, signature) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    {space, device, seq, Hlc::normalize(op.hlc), ChangeLog::internId(library, op.author), op.entity,
                     ChangeLog::internId(library, op.id), op.field, op.value,
                     isSigned ? QVariant(ChangeLog::internId(library, op.signer)) : QVariant(),
                     isSigned ? QVariant(op.signature) : QVariant()});
    ChangeLog::exec(library,
                    "INSERT INTO seen (space, device, seq, hlc) VALUES (?, ?, ?, ?) ON CONFLICT(space, device) DO UPDATE SET "
                    "seq = MAX(seq, excluded.seq), hlc = MAX(hlc, excluded.hlc)",
                    {space, device, seq, Hlc::normalize(op.hlc)});
}

// Where the books of one space are here, apart from every other space's.
[[nodiscard]] QString booksOf(const QString &space) { return u"subscribed-book:"_s + space; }

} // namespace

std::expected<void, QString> SubscribedSpace::verify(const Library &library, const SpaceSnapshot &snapshot) {
    if (snapshot.kind != QLatin1StringView(PublishedSpace::kind) || !snapshot.space.startsWith("published:"_L1))
        return std::unexpected(
            u"This is not a published space (it is of the kind \"%1\"), so it can't be subscribed to."_s.arg(snapshot.kind));
    if (!Person::publicKeyFrom(snapshot.owner.person))
        return std::unexpected(u"This space doesn't say who publishes it: its owner's id is not a person's key."_s);
    const auto stranger = std::ranges::find_if(snapshot.operations, [&snapshot](const SpaceOperation &op) {
        return op.space != snapshot.space || op.author != snapshot.owner.person || op.device.isEmpty() || op.seq <= 0 || op.entity.isEmpty()
               || op.id.isEmpty();
    });
    if (stranger != snapshot.operations.cend())
        return std::unexpected(u"Operation %1 of this space is not its owner's, so nothing of it was taken in."_s.arg(
            std::ranges::distance(snapshot.operations.cbegin(), stranger) + 1));
    // A signed operation must be the owner's and as they signed it, by the
    // check replication makes of what a peer sends (SignedOps::verify). A
    // published space's operations go unsigned for now (#48 publishes them
    // without the signatures they had at home), and a subscribed space is
    // read-only, never written back, so an unsigned one is taken in.
    const auto forged = std::ranges::find_if(snapshot.operations, [&snapshot](const SpaceOperation &op) {
        return !op.signature.isEmpty() && (op.signer != snapshot.owner.person || !SignedOps::verify(op));
    });
    if (forged != snapshot.operations.cend())
        return std::unexpected(u"Operation %1 of this space is not as its owner signed it, so nothing of it was taken in."_s.arg(
            std::ranges::distance(snapshot.operations.cbegin(), forged) + 1));
    if (spaceKnown(library, snapshot.space) && !isSubscribed(library, snapshot.space))
        return std::unexpected(u"%1 is a space this library publishes itself, so it can't be subscribed to here."_s.arg(snapshot.name));
    return {};
}

bool SubscribedSpace::isSubscribed(const Library &library, const QString &space) {
    QSqlQuery query = ChangeLog::query(library, "SELECT 1 FROM space_members WHERE space = ? AND person = ? AND role = ?",
                                       {space, Spaces::deviceId(library), QString::fromLatin1(readerRole)});
    return query.next();
}

QList<SubscribedSpace::Subscription> SubscribedSpace::list(const Library &library) {
    QList<Subscription> subscriptions;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT s.id, s.name, s.owner, COALESCE((SELECT o.name FROM space_members AS o "
                                            "WHERE o.space = s.id AND o.person = s.owner), '') FROM spaces AS s "
                                            "JOIN space_members AS m ON m.space = s.id WHERE m.person = ? AND m.role = ? ORDER BY s.name",
                                            {Spaces::deviceId(library), QString::fromLatin1(readerRole)});
         query.next();)
        subscriptions << Subscription{.space = query.value(0).toString(),
                                      .name = query.value(1).toString(),
                                      .owner = query.value(2).toString(),
                                      .ownerName = query.value(3).toString(),
                                      .collection = 0};
    for (Subscription &subscription : subscriptions)
        subscription.collection = linkedCollection(library, subscription.space);
    return subscriptions;
}

QList<SpaceOperation> SubscribedSpace::operations(const Library &library, const QString &space) {
    QList<SpaceOperation> list;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value FROM space_ops AS o "
                                            "JOIN log_ids AS d ON d.id = o.device JOIN log_ids AS a ON a.id = o.author "
                                            "JOIN log_ids AS i ON i.id = o.id WHERE o.space = (SELECT id FROM log_ids WHERE name = ?) "
                                            "ORDER BY d.name, o.seq",
                                            {space});
         query.next();)
        list.append({.space = space,
                     .device = query.value(0).toString(),
                     .seq = query.value(1).toLongLong(),
                     .hlc = query.value(2).toLongLong(),
                     .author = query.value(3).toString(),
                     .entity = query.value(4).toString(),
                     .id = query.value(5).toString(),
                     .field = query.value(6).toString(),
                     .value = query.value(7).toString()});
    return list;
}

QList<SpaceOperation> SubscribedSpace::lacking(const Library &library, const SpaceSnapshot &snapshot) {
    const Held here = held(library, snapshot.space);
    QSet<QString> taken;
    QList<SpaceOperation> lacking;
    for (const SpaceOperation &op : snapshot.operations)
        if (const QString same = sameness(op); !here.operations.contains(same) && !taken.contains(same)) {
            taken.insert(same);
            lacking << op;
        }
    return lacking;
}

std::expected<SubscribedSpace::Accepted, QString> SubscribedSpace::accept(const Library &library, const SpaceSnapshot &snapshot) {
    if (const auto verified = verify(library, snapshot); !verified)
        return std::unexpected(verified.error());
    const QList<SpaceOperation> lacking = SubscribedSpace::lacking(library, snapshot);
    const Library::Batch batch(library);
    Spaces::ensurePublishedSpace(library, snapshot.space, snapshot.name, snapshot.owner.person, snapshot.owner.name);
    // The owner may have renamed the collection since.
    ChangeLog::exec(library, "UPDATE spaces SET name = ? WHERE id = ?", {snapshot.name, snapshot.space});
    ChangeLog::exec(library, "INSERT OR IGNORE INTO space_members (space, person, role, added) VALUES (?, ?, ?, ?)",
                    {snapshot.space, Spaces::deviceId(library), QString::fromLatin1(readerRole), Spaces::lastHlc(library)});
    // The owner's clock is not this library's: nothing of the space merges
    // with its own writes, so the library's clock is left as it is.
    QHash<QString, qint64> last = held(library, snapshot.space).last;
    for (const SpaceOperation &op : lacking) {
        qint64 &device = last[op.device];
        device = op.seq > device ? op.seq : device + 1;
        keep(library, op, device);
    }
    return Accepted{.added = lacking.size(), .known = snapshot.operations.size() - lacking.size()};
}

void SubscribedSpace::link(const Library &library, const QString &space, qint64 collection) {
    Spaces::setGlobalId(library, u"subscription"_s, collection, space);
}

qint64 SubscribedSpace::linkedCollection(const Library &library, const QString &space) {
    const qint64 collection = Spaces::localId(library, u"subscription"_s, space);
    const std::optional<Collection> there = collection > 0 ? library.collection(collection) : std::nullopt;
    return there && !there->removed ? collection : 0;
}

void SubscribedSpace::placeBook(const Library &library, const QString &space, const QString &book, qint64 here) {
    if (here > 0)
        Spaces::setGlobalId(library, booksOf(space), here, book);
}

qint64 SubscribedSpace::bookHere(const Library &library, const QString &space, const QString &book) {
    const qint64 placed = Spaces::localId(library, booksOf(space), book);
    const qint64 here = placed > 0 ? placed : Spaces::localId(library, u"book"_s, book);
    return here > 0 && library.book(here) ? here : 0;
}

std::expected<qint64, QString> SubscribedSpace::drop(const Library &library, const QString &space) {
    if (!isSubscribed(library, space))
        return std::unexpected(u"This library doesn't subscribe to %1."_s.arg(space));
    const Library::Batch batch(library);
    const qint64 dropped = Compaction::forgetSubscribed(library, space);
    ChangeLog::exec(library, "DELETE FROM space_members WHERE space = ?", {space});
    ChangeLog::exec(library, "DELETE FROM spaces WHERE id = ?", {space});
    ChangeLog::exec(library, "DELETE FROM global_ids WHERE entity = 'subscription' AND global_id = ?", {space});
    ChangeLog::exec(library, "DELETE FROM global_ids WHERE entity = ?", {booksOf(space)});
    return dropped;
}
