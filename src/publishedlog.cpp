#include "publishedlog.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <tuple>

#include "changelog.h"
#include "compaction.h"
#include "library.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString keyOf(const QString &entity, const QString &id) { return entity + u'\x1f' + id; }
[[nodiscard]] QString grownOf(const SpaceOperation &op) { return op.field + u'\x1f' + op.value; }

// A thing's tombstone, as SpaceReducer reads one: of a book any value but
// "0" or none, of the rest "1".
[[nodiscard]] bool removes(const SpaceOperation &op) {
    if (op.field != "removed"_L1)
        return false;
    return op.entity == "book"_L1 ? !op.value.isEmpty() && op.value != "0"_L1 : op.value == "1"_L1;
}

// Every kind a space holds can go, a reading day too: what is switched off
// is taken back from those who had it, not only no longer sent.
[[nodiscard]] bool removable(const QString &entity) {
    return entity == "book"_L1 || entity == "collection"_L1 || entity == "node"_L1 || entity == "fact"_L1 || entity == "edge"_L1
           || entity == "reading_log"_L1;
}

// What a removal of "0" brings back; a fact, a relation or a reading day
// comes back by saying what it says again, later than its removal.
[[nodiscard]] bool backByZero(const QString &entity) { return entity == "book"_L1 || entity == "collection"_L1 || entity == "node"_L1; }

struct Said {
    qint64 hlc = -1;
    qint64 seq = 0;
    QString value;
};

// What the log says of one thing so far.
struct Logged {
    qint64 alive = -1;
    qint64 removed = -1;
    QHash<QString, Said> fields; // of each, the latest
    QSet<QString> grown;         // of what only grows, each field and value

    [[nodiscard]] bool gone() const { return removed >= 0 && removed >= alive; }
};

[[nodiscard]] QHash<QString, Logged> loggedOf(std::span<const SpaceOperation> ops) {
    QHash<QString, Logged> things;
    for (const SpaceOperation &op : ops) {
        Logged &thing = things[keyOf(op.entity, op.id)];
        if (removes(op)) {
            thing.removed = std::max(thing.removed, op.hlc);
            continue;
        }
        thing.alive = std::max(thing.alive, op.hlc);
        if (op.field == "removed"_L1)
            continue;
        if (SpaceReducer::grows(op.field)) {
            thing.grown.insert(grownOf(op));
            continue;
        }
        Said &said = thing.fields[op.field];
        if (std::tie(op.hlc, op.seq) > std::tie(said.hlc, said.seq))
            said = {.hlc = op.hlc, .seq = op.seq, .value = op.value};
    }
    return things;
}

// What the space says now of one thing: of each field the value that wins,
// and what only grows.
struct Now {
    QString entity;
    QString id;
    QMap<QString, SpaceOperation> fields;
    QList<SpaceOperation> grown;
};

// The order SpaceReducer gives values of one field: by stamp, an add over a
// concurrent remove of a book in a collection, then by device and value.
[[nodiscard]] auto rank(const SpaceOperation &op) {
    int priority = 1;
    if (op.field.startsWith("book:"_L1) && op.value != "out"_L1)
        priority = op.value == "in"_L1 ? 2 : 0;
    return std::tuple<qint64, int, const QString &, const QString &>(op.hlc, priority, op.device, op.value);
}

[[nodiscard]] QMap<QString, Now> nowOf(std::span<const SpaceOperation> ops) {
    QMap<QString, Now> things;
    for (const SpaceOperation &op : ops) {
        Now &thing = things[keyOf(op.entity, op.id)];
        thing.entity = op.entity;
        thing.id = op.id;
        if (SpaceReducer::grows(op.field)) {
            thing.grown << op;
            continue;
        }
        const auto at = thing.fields.constFind(op.field);
        if (at == thing.fields.cend() || rank(op) > rank(*at))
            thing.fields.insert(op.field, op);
    }
    return things;
}

// An operation to append; `fresh` when it takes a new stamp, later than all
// the log has, rather than its own.
struct Append {
    SpaceOperation op;
    bool fresh = false;
};

[[nodiscard]] SpaceOperation said(const QString &entity, const QString &id, const QString &field, const QString &value) {
    return {.space = {}, .device = {}, .seq = 0, .hlc = 0, .author = {}, .entity = entity, .id = id, .field = field, .value = value};
}

void fieldsOf(const Now &now, const Logged &was, QList<Append> &out) {
    for (const SpaceOperation &op : now.fields) {
        const auto before = was.fields.constFind(op.field);
        if (before == was.fields.cend()) {
            out << Append{.op = op, .fresh = false};
            continue;
        }
        // A stamp not after the log's own would lose to the value it replaces.
        if (before->value != op.value)
            out << Append{.op = op, .fresh = op.hlc <= before->hlc};
    }
    // A field no longer said (a part switched off) is said to be empty.
    for (auto field = was.fields.cbegin(); field != was.fields.cend(); ++field)
        if (!now.fields.contains(field.key()) && !field->value.isEmpty())
            out << Append{.op = said(now.entity, now.id, field.key(), {}), .fresh = true};
}

void followThing(const Now &now, const Logged &was, bool known, QList<Append> &out) {
    const bool back = known && was.gone();
    fieldsOf(now, was, out);
    const bool sayAgain = back && !backByZero(now.entity);
    for (const SpaceOperation &op : now.grown)
        if (sayAgain || !was.grown.contains(grownOf(op)))
            out << Append{.op = op, .fresh = sayAgain};
    if (back && backByZero(now.entity))
        out << Append{.op = said(now.entity, now.id, u"removed"_s, u"0"_s), .fresh = true};
}

[[nodiscard]] QList<Append> changes(std::span<const SpaceOperation> logged, std::span<const SpaceOperation> now) {
    const QHash<QString, Logged> was = loggedOf(logged);
    const QMap<QString, Now> is = nowOf(now);
    QList<Append> out;
    for (const Now &thing : is) {
        const auto before = was.constFind(keyOf(thing.entity, thing.id));
        followThing(thing, before == was.cend() ? Logged{} : *before, before != was.cend(), out);
    }
    // What is no longer in the space goes, of what can go.
    QStringList gone;
    for (auto thing = was.cbegin(); thing != was.cend(); ++thing)
        if (!is.contains(thing.key()) && !thing->gone() && removable(thing.key().section(u'\x1f', 0, 0)))
            gone << thing.key();
    gone.sort();
    for (const QString &key : std::as_const(gone))
        out << Append{.op = said(key.section(u'\x1f', 0, 0), key.section(u'\x1f', 1), u"removed"_s, u"1"_s), .fresh = true};
    // In an order that is the same each time: what keeps its stamp by it,
    // then what is stamped now.
    std::ranges::stable_sort(out, [](const Append &one, const Append &other) {
        return std::tie(one.fresh, one.op.hlc, one.op.entity, one.op.id, one.op.field, one.op.value)
               < std::tie(other.fresh, other.op.hlc, other.op.entity, other.op.id, other.op.field, other.op.value);
    });
    return out;
}

[[nodiscard]] qint64 lastSeq(const Library &library, const QString &space, const QString &device) {
    QSqlQuery query =
        ChangeLog::query(library, "SELECT COALESCE(MAX(seq), 0) FROM published_ops WHERE space = ? AND device = ?", {space, device});
    return query.next() ? query.value(0).toLongLong() : 0;
}

} // namespace

void PublishedLog::remember(const Library &library, const QString &space, const Record &record) {
    ChangeLog::exec(library,
                    "INSERT INTO published (space, collection, parts) VALUES (?, ?, ?) "
                    "ON CONFLICT(space) DO UPDATE SET collection = excluded.collection, parts = excluded.parts",
                    {space, record.collection, record.parts.join(u',')});
}

std::optional<PublishedLog::Record> PublishedLog::recorded(const Library &library, const QString &space) {
    QSqlQuery query = ChangeLog::query(library, "SELECT collection, parts FROM published WHERE space = ?", {space});
    if (!query.next())
        return std::nullopt;
    return Record{.collection = query.value(0).toString(), .parts = query.value(1).toString().split(u',', Qt::SkipEmptyParts)};
}

QString PublishedLog::deviceIn(const Library &library, const QString &space) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(space.toUtf8());
    hash.addData(QByteArrayView("\x1f", 1));
    hash.addData(Spaces::deviceId(library).toUtf8());
    return u"device-"_s + QString::fromLatin1(hash.result().toHex().left(12));
}

QList<SpaceOperation> PublishedLog::operations(const Library &library, const QString &space) {
    QList<SpaceOperation> ops;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT device, seq, hlc, author, entity, id, field, value FROM published_ops "
                                            "WHERE space = ? ORDER BY device, seq",
                                            {space});
         query.next();)
        ops << SpaceOperation{.space = space,
                              .device = query.value(0).toString(),
                              .seq = query.value(1).toLongLong(),
                              .hlc = query.value(2).toLongLong(),
                              .author = query.value(3).toString(),
                              .entity = query.value(4).toString(),
                              .id = query.value(5).toString(),
                              .field = query.value(6).toString(),
                              .value = query.value(7).toString()};
    return ops;
}

QList<SpaceOperation> PublishedLog::compacted(std::span<const SpaceOperation> ops) {
    const QHash<QString, Logged> things = loggedOf(ops);
    QHash<QString, SpaceOperation> tombstones;
    QList<SpaceOperation> kept;
    for (const SpaceOperation &op : Compaction::latest(ops)) {
        const QString key = keyOf(op.entity, op.id);
        if (!things.value(key).gone()) {
            kept << op;
            continue;
        }
        // Of a thing gone, its latest removal and nothing it said.
        const auto at = tombstones.constFind(key);
        if (removes(op) && (at == tombstones.cend() || std::tie(op.hlc, op.seq) > std::tie(at->hlc, at->seq)))
            tombstones.insert(key, op);
    }
    kept << tombstones.values();
    std::ranges::sort(kept, [](const SpaceOperation &one, const SpaceOperation &other) {
        return std::tie(one.device, one.seq) < std::tie(other.device, other.seq);
    });
    return kept;
}

qint64 PublishedLog::follow(const Library &library, const QString &space, std::span<const SpaceOperation> now, const QString &author) {
    const Library::Batch batch(library);
    const QList<Append> appends = changes(operations(library, space), now);
    const QString device = deviceIn(library, space);
    qint64 seq = lastSeq(library, space, device);
    for (const Append &append : appends) {
        const SpaceOperation &op = append.op;
        const qint64 hlc = append.fresh ? Spaces::tickHlc(library) : op.hlc;
        ChangeLog::exec(library,
                        "INSERT INTO published_ops (space, device, seq, hlc, author, entity, id, field, value) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        {space, device, ++seq, hlc, author, op.entity, op.id, op.field, op.value});
    }
    return appends.size();
}

qint64 PublishedLog::handOver(const Library &library, const QString &from, const QString &to) {
    if (from.isEmpty() || to.isEmpty() || from == to)
        return 0;
    const Library::Batch batch(library);
    QStringList spaces;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT s.id FROM spaces AS s JOIN published AS p ON p.space = s.id "
                                            "WHERE s.kind = 'published' AND s.owner = ? ORDER BY s.id",
                                            {from});
         query.next();)
        spaces << query.value(0).toString();
    for (const QString &space : std::as_const(spaces)) {
        const QString device = deviceIn(library, space);
        ChangeLog::exec(library,
                        "INSERT INTO published_ops (space, device, seq, hlc, author, entity, id, field, value) "
                        "VALUES (?, ?, ?, ?, ?, 'owner', ?, 'handed_to', ?)",
                        {space, device, lastSeq(library, space, device) + 1, Spaces::tickHlc(library), from, from, to});
        ChangeLog::exec(library, "UPDATE spaces SET owner = ? WHERE id = ?", {to, space});
        // The name they go by in it goes with it.
        ChangeLog::exec(library,
                        "INSERT INTO space_members (space, person, role, name, added) SELECT space, ?, role, name, ? FROM space_members "
                        "WHERE space = ? AND person = ? ON CONFLICT(space, person) DO UPDATE SET name = excluded.name",
                        {to, Spaces::lastHlc(library), space, from});
    }
    return spaces.size();
}

QSet<QString> PublishedLog::owners(const QString &owner, std::span<const SpaceOperation> ops) {
    QSet<QString> owners{owner};
    // Each hand-over is from whoever wrote it, to one who owned it later.
    for (bool grew = true; grew;) {
        const qsizetype before = owners.size();
        for (const SpaceOperation &op : ops)
            if (op.entity == "owner"_L1 && op.field == "handed_to"_L1 && op.author == op.id && owners.contains(op.value))
                owners.insert(op.id);
        grew = owners.size() != before;
    }
    return owners;
}
