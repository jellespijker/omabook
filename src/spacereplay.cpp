#include "spacereplay.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <utility>

#include "changelog.h"
#include "hlc.h"
#include "library.h"
#include "replication.h"
#include "spacereducer.h"
#include "spaces.h"
#include "tenancy.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QJsonObject parseObject(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

[[nodiscard]] QJsonArray parseArray(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).array(); }

[[nodiscard]] QString compactJson(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString fieldText(const QHash<QString, SpaceStamp> &fields, const QString &key, const QString &fallback = {}) {
    const auto it = fields.constFind(key);
    return it != fields.cend() ? it->value : fallback;
}

[[nodiscard]] QString mergedTags(const ReducedBook &book) {
    QStringList tags = BookMeta::splitTags(fieldText(book.fields, u"tags"_s));
    for (auto it = book.tagSet.cbegin(); it != book.tagSet.cend(); ++it) {
        const bool present = it.value().value == "1"_L1;
        if (present && !tags.contains(it.key(), Qt::CaseInsensitive))
            tags << it.key();
        else if (!present)
            tags.removeIf([&](const QString &t) { return t.compare(it.key(), Qt::CaseInsensitive) == 0; });
    }
    return tags.join(", "_L1);
}

void writeBookRow(const Library &library, const QString &gid, const ReducedBook &book, QSet<qint64> &keptIds) {
    const qint64 detailsHlc = SpaceReducer::detailsHlc(book);
    const qint64 readingHlc = SpaceReducer::readingHlc(book);
    const QVariantList values{fieldText(book.fields, u"title"_s, u"Untitled"_s),
                              fieldText(book.fields, u"authors"_s),
                              fieldText(book.fields, u"series"_s),
                              fieldText(book.fields, u"series_index"_s, u"0"_s).toDouble(),
                              fieldText(book.fields, u"publisher"_s),
                              fieldText(book.fields, u"language"_s),
                              fieldText(book.fields, u"isbn"_s),
                              fieldText(book.fields, u"published"_s),
                              fieldText(book.fields, u"description"_s),
                              mergedTags(book),
                              fieldText(book.fields, u"path"_s),
                              fieldText(book.fields, u"added"_s, u"0"_s).toLongLong(),
                              fieldText(book.fields, u"has_cover"_s) == "1"_L1 ? 1 : 0,
                              fieldText(book.fields, u"paper"_s) == "1"_L1 ? 1 : 0,
                              fieldText(book.fields, u"shelf"_s),
                              fieldText(book.fields, u"rating"_s, u"0"_s).toInt(),
                              fieldText(book.fields, u"finished"_s),
                              fieldText(book.fields, u"identifiers"_s, u"{}"_s),
                              Hlc::seconds(detailsHlc),
                              Hlc::seconds(readingHlc)};

    qint64 localId = Spaces::localId(library, u"book"_s, gid);
    if (localId > 0 && !ChangeLog::query(library, "SELECT 1 FROM books WHERE id = ?", {localId}).next())
        localId = 0;
    if (localId == 0) {
        const auto made =
            ChangeLog::insert(library,
                              "INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                              "description, tags, path, added, has_cover, paper, shelf, rating, finished, identifiers, details_at, "
                              "reading_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                              values);
        if (!made)
            return;
        localId = *made;
        Spaces::setGlobalId(library, u"book"_s, localId, gid);
    } else {
        QVariantList updateValues = values;
        updateValues << localId;
        ChangeLog::exec(library,
                        "UPDATE books SET title = ?, authors = ?, series = ?, series_index = ?, publisher = ?, "
                        "language = ?, isbn = ?, published = ?, description = ?, tags = ?, path = ?, added = ?, "
                        "has_cover = ?, paper = ?, shelf = ?, rating = ?, finished = ?, identifiers = ?, details_at = ?, "
                        "reading_at = ? WHERE id = ?",
                        updateValues);
    }
    keptIds.insert(localId);
    if (book.fields.contains(u"formats"_s)) {
        ChangeLog::exec(library, "DELETE FROM formats WHERE book_id = ?", {localId});
        for (const auto &entry : parseArray(fieldText(book.fields, u"formats"_s))) {
            const QJsonObject f = entry.toObject();
            ChangeLog::exec(library, "INSERT OR REPLACE INTO formats (book_id, format, file, size, hash) VALUES (?, ?, ?, ?, ?)",
                            {localId, f.value("format").toString(), f.value("file").toString(), f.value("size").toInteger(),
                             f.value("hash").toString()});
        }
    }
    if (const auto prog = book.fields.constFind(u"progress"_s); prog != book.fields.cend() && !prog->value.isEmpty()) {
        const QJsonObject p = parseObject(prog->value);
        ChangeLog::exec(library, "INSERT OR REPLACE INTO progress (book_id, section, fraction, percent, updated) VALUES (?, ?, ?, ?, ?)",
                        {localId, p.value("section").toInt(), p.value("fraction").toDouble(), p.value("percent").toDouble(),
                         Hlc::seconds(SpaceReducer::fieldHlc(book, u"progress"_s))});
    }
}

void dropStaleRows(const Library &library, const QString &entity, const QString &table, const QSet<qint64> &keptIds) {
    QList<qint64> stale;
    for (QSqlQuery query = ChangeLog::query(library, "SELECT id FROM " + table); query.next();)
        if (const qint64 id = query.value(0).toLongLong(); !keptIds.contains(id))
            stale << id;
    for (const qint64 id : std::as_const(stale)) {
        ChangeLog::exec(library, "DELETE FROM global_ids WHERE entity = ? AND local_id = ?", {entity, id});
        ChangeLog::exec(library, "DELETE FROM " + table + " WHERE id = ?", {id});
    }
}

void materializeBooks(const Library &library, const QMap<QString, ReducedBook> &books) {
    ChangeLog::exec(library, "DELETE FROM removed");
    QSet<qint64> keptIds;
    for (auto it = books.cbegin(); it != books.cend(); ++it) {
        const QString &gid = it.key();
        const ReducedBook &book = it.value();
        if (SpaceReducer::isRemoved(book.alive, book.removed)) {
            const QString key = book.removed.value.isEmpty() ? gid.mid(2) : book.removed.value;
            ChangeLog::exec(library, "INSERT OR REPLACE INTO removed (hash, removed_at) VALUES (?, ?)",
                            {key, Hlc::seconds(SpaceReducer::fieldHlc(book, u"removed"_s))});
            continue;
        }
        writeBookRow(library, gid, book, keptIds);
    }
    dropStaleRows(library, u"book"_s, u"books"_s, keptIds);
}

void writeCollectionBooks(const Library &library, qint64 localId, const ReducedCollection &col) {
    for (auto bit = col.books.cbegin(); bit != col.books.cend(); ++bit) {
        if (bit.value().value != "in"_L1 && bit.value().value != "out"_L1)
            continue;
        if (const qint64 bookId = Spaces::localId(library, u"book"_s, bit.key()); bookId > 0)
            ChangeLog::exec(library, "INSERT OR REPLACE INTO collection_books (collection_id, book_id, included) VALUES (?, ?, ?)",
                            {localId, bookId, bit.value().value == "in"_L1 ? 1 : 0});
    }
}

void writeCollectionRow(const Library &library, const QString &gid, const ReducedCollection &col, int fallbackPos, QSet<qint64> &keptIds) {
    const bool removed = SpaceReducer::isRemoved(col.alive, col.removed);
    const qint64 updated = Hlc::seconds(std::max(col.alive.hlc, col.removed.hlc));
    const QString fallbackName = gid.startsWith("c:"_L1) ? gid.mid(2).section('/', -1) : gid;
    const QString name = fieldText(col.fields, u"name"_s, fallbackName);
    const QString parentGid = fieldText(col.fields, u"parent"_s);
    const qint64 parentId = parentGid.isEmpty() ? 0 : Spaces::localId(library, u"collection"_s, parentGid);
    const QString rule = removed ? QString() : fieldText(col.fields, u"rule"_s);
    const QString shelf = removed ? u"{}"_s : fieldText(col.fields, u"shelf"_s, u"{}"_s);
    const QString format = removed ? u"{}"_s : fieldText(col.fields, u"format"_s, u"{}"_s);
    const int position = fieldText(col.fields, u"position"_s, QString::number(fallbackPos)).toInt();

    qint64 localId = Spaces::localId(library, u"collection"_s, gid);
    if (localId > 0 && !ChangeLog::query(library, "SELECT 1 FROM collections WHERE id = ?", {localId}).next())
        localId = 0;
    if (localId == 0) {
        QSqlQuery byName =
            ChangeLog::query(library, "SELECT id FROM collections WHERE parent = ? AND name = ? COLLATE NOCASE", {parentId, name});
        if (byName.next())
            localId = byName.value(0).toLongLong();
    }
    if (localId == 0) {
        const auto made = ChangeLog::insert(library,
                                            "INSERT INTO collections (name, rule, position, shelf, format, updated, removed, parent) "
                                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                                            {name, rule, position, shelf, format, updated, removed ? 1 : 0, parentId});
        if (!made)
            return;
        localId = *made;
    } else {
        ChangeLog::exec(library,
                        "UPDATE collections SET name = ?, rule = ?, position = ?, shelf = ?, format = ?, "
                        "updated = ?, removed = ?, parent = ? WHERE id = ?",
                        {name, rule, position, shelf, format, updated, removed ? 1 : 0, parentId, localId});
    }
    Spaces::setGlobalId(library, u"collection"_s, localId, gid);
    keptIds.insert(localId);
    ChangeLog::exec(library, "DELETE FROM collection_books WHERE collection_id = ?", {localId});
    if (!removed)
        writeCollectionBooks(library, localId, col);
}

void materializeCollections(const Library &library, const QMap<QString, ReducedCollection> &collections) {
    QSet<qint64> keptIds;
    int fallbackPos = 1;
    for (auto it = collections.cbegin(); it != collections.cend(); ++it)
        writeCollectionRow(library, it.key(), it.value(), fallbackPos++, keptIds);
    dropStaleRows(library, u"collection"_s, u"collections"_s, keptIds);
}

[[nodiscard]] QString mergedNodeData(const ReducedNode &node) {
    QJsonObject data = parseObject(fieldText(node.fields, u"data"_s, u"{}"_s));
    if (node.aliases.isEmpty())
        return compactJson(data);
    QStringList aliases;
    for (const auto &existing : data.value("aliases").toArray())
        aliases << existing.toString();
    for (auto ait = node.aliases.cbegin(); ait != node.aliases.cend(); ++ait) {
        if (ait.value().value == "1"_L1 && !aliases.contains(ait.key(), Qt::CaseInsensitive))
            aliases << ait.key();
        else if (ait.value().value != "1"_L1)
            aliases.removeIf([&](const QString &a) { return a.compare(ait.key(), Qt::CaseInsensitive) == 0; });
    }
    if (aliases.isEmpty())
        data.remove("aliases");
    else
        data.insert("aliases", QJsonArray::fromStringList(aliases));
    return compactJson(data);
}

void writeNodeRow(const Library &library, const QString &gid, const ReducedNode &node, QSet<qint64> &keptIds) {
    if (SpaceReducer::isRemoved(node.alive, node.removed))
        return;
    const QString scope = fieldText(node.fields, u"scope"_s);
    const QString kind = fieldText(node.fields, u"kind"_s);
    const QString name = fieldText(node.fields, u"name"_s);
    if (kind.isEmpty() || name.isEmpty())
        return;
    const QString summary = fieldText(node.fields, u"summary"_s);
    const QString dataText = mergedNodeData(node);
    const QString bookGid = fieldText(node.fields, u"book"_s);
    const qint64 bookId = bookGid.isEmpty() ? 0 : Spaces::localId(library, u"book"_s, bookGid);
    const QVariant bookCol = bookId > 0 ? QVariant(bookId) : QVariant(QMetaType(QMetaType::LongLong));
    const qint64 updated = Hlc::seconds(std::max<qint64>(0, node.alive.hlc));

    qint64 localId = Spaces::localId(library, u"node"_s, gid);
    if (localId > 0 && !ChangeLog::query(library, "SELECT 1 FROM graph_nodes WHERE id = ?", {localId}).next())
        localId = 0;
    if (localId == 0) {
        const auto made = ChangeLog::insert(
            library, "INSERT INTO graph_nodes (scope, kind, name, summary, data, book_id, updated) VALUES (?, ?, ?, ?, ?, ?, ?)",
            {scope, kind, name, summary, dataText, bookCol, updated});
        if (!made)
            return;
        localId = *made;
    } else {
        ChangeLog::exec(library,
                        "UPDATE graph_nodes SET scope = ?, kind = ?, name = ?, summary = ?, data = ?, "
                        "book_id = ?, updated = ? WHERE id = ?",
                        {scope, kind, name, summary, dataText, bookCol, updated, localId});
    }
    Spaces::setGlobalId(library, u"node"_s, localId, gid);
    keptIds.insert(localId);
    ChangeLog::exec(library, "DELETE FROM graph_seen WHERE node_id = ?", {localId});
    for (auto sit = node.seen.cbegin(); sit != node.seen.cend(); ++sit) {
        if (const qint64 seenBook = Spaces::localId(library, u"book"_s, sit.key()); seenBook > 0)
            ChangeLog::exec(library,
                            "INSERT OR REPLACE INTO graph_seen (node_id, book_id, section, fraction, anchor) "
                            "VALUES (?, ?, ?, ?, ?)",
                            {localId, seenBook, sit.value().place.section, sit.value().place.fraction, sit.value().anchor});
    }
}

void materializeNodes(const Library &library, const QMap<QString, ReducedNode> &nodes) {
    QSet<qint64> keptIds;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        writeNodeRow(library, it.key(), it.value(), keptIds);
    dropStaleRows(library, u"node"_s, u"graph_nodes"_s, keptIds);
}

void materializeFacts(const Library &library, const QMap<QString, ReducedFact> &facts) {
    ChangeLog::exec(library, "DELETE FROM graph_facts");
    ChangeLog::exec(library, "DELETE FROM global_ids WHERE entity = 'fact'");
    for (auto it = facts.cbegin(); it != facts.cend(); ++it) {
        const ReducedFact &fact = it.value();
        if (SpaceReducer::isRemoved(fact.alive, fact.removed))
            continue;
        const qint64 nodeId = Spaces::localId(library, u"node"_s, fact.nodeGid);
        if (nodeId == 0 || fact.kind.isEmpty() || fact.value.isEmpty())
            continue;
        const qint64 bookId = fact.bookGid.isEmpty() ? 0 : Spaces::localId(library, u"book"_s, fact.bookGid);
        const QVariant bookCol = bookId > 0 ? QVariant(bookId) : QVariant(QMetaType(QMetaType::LongLong));
        if (const auto made =
                ChangeLog::insert(library,
                                  "INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction, anchor, tenant) "
                                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                                  {nodeId, fact.kind, fact.value, bookCol, fact.moment.place.section, fact.moment.place.fraction,
                                   fact.moment.anchor, bookId > 0 || fact.tenant.isNull() ? QString("") : fact.tenant}))
            Spaces::setGlobalId(library, u"fact"_s, *made, it.key());
    }
}

void materializeEdges(const Library &library, const QMap<QString, ReducedEdge> &edges) {
    ChangeLog::exec(library, "DELETE FROM graph_edges");
    ChangeLog::exec(library, "DELETE FROM global_ids WHERE entity = 'edge'");
    for (auto it = edges.cbegin(); it != edges.cend(); ++it) {
        const ReducedEdge &edge = it.value();
        if (SpaceReducer::isRemoved(edge.alive, edge.removed))
            continue;
        const qint64 sourceId = Spaces::localId(library, u"node"_s, edge.sourceGid);
        const qint64 targetId = Spaces::localId(library, u"node"_s, edge.targetGid);
        if (sourceId == 0 || targetId == 0 || sourceId == targetId || edge.kind.isEmpty())
            continue;
        const qint64 bookId = edge.bookGid.isEmpty() ? 0 : Spaces::localId(library, u"book"_s, edge.bookGid);
        if (const auto made = ChangeLog::insert(
                library,
                "INSERT OR REPLACE INTO graph_edges (source, target, kind, note, book_id, section, fraction, anchor, notes) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {sourceId, targetId, edge.kind, edge.note.value, bookId, edge.learned.place.section, edge.learned.place.fraction,
                 edge.learned.anchor, edge.notes.value.isNull() ? QString("") : edge.notes.value}))
            Spaces::setGlobalId(library, u"edge"_s, *made, it.key());
    }
}

void materializeDays(const Library &library, const QMap<QString, ReducedDay> &days) {
    ChangeLog::exec(library, "DELETE FROM reading_log");
    for (auto it = days.cbegin(); it != days.cend(); ++it) {
        const ReducedDay &day = it.value();
        if (!day.valid)
            continue;
        if (const qint64 bookId = Spaces::localId(library, u"book"_s, day.bookGid); bookId > 0)
            ChangeLog::exec(library, "INSERT OR REPLACE INTO reading_log (book_id, day, from_percent, to_percent) VALUES (?, ?, ?, ?)",
                            {bookId, day.day, day.from, day.to});
    }
}

[[nodiscard]] ReducedBook reducedBookFor(const Library &library, const QString &gid) {
    ReducedBook reduced;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT s.name, d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value "
                                            "FROM live_ops AS o JOIN log_ids AS s ON s.id = o.space JOIN log_ids AS d ON d.id = o.device "
                                            "JOIN log_ids AS a ON a.id = o.author JOIN log_ids AS i ON i.id = o.id "
                                            "WHERE o.entity = 'book' AND o.id = (SELECT id FROM log_ids WHERE name = ?)",
                                            {gid});
         query.next();) {
        SpaceReducer::foldBook(reduced, {.space = query.value(0).toString(),
                                         .device = query.value(1).toString(),
                                         .seq = query.value(2).toLongLong(),
                                         .hlc = query.value(3).toLongLong(),
                                         .author = query.value(4).toString(),
                                         .entity = query.value(5).toString(),
                                         .id = query.value(6).toString(),
                                         .field = query.value(7).toString(),
                                         .value = query.value(8).toString()});
    }
    return reduced;
}

} // namespace

QList<SpaceOperation> SpaceReplay::operations(const Library &library) {
    QList<SpaceOperation> list;
    for (QSqlQuery query = ChangeLog::query(
             library, "SELECT s.name, d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value, g.name, o.signature "
                      "FROM live_ops AS o JOIN log_ids AS s ON s.id = o.space JOIN log_ids AS d ON d.id = o.device "
                      "JOIN log_ids AS a ON a.id = o.author JOIN log_ids AS i ON i.id = o.id "
                      "LEFT JOIN log_ids AS g ON g.id = o.signer ORDER BY s.name, d.name, o.seq");
         query.next();) {
        list.append({.space = query.value(0).toString(),
                     .device = query.value(1).toString(),
                     .seq = query.value(2).toLongLong(),
                     .hlc = query.value(3).toLongLong(),
                     .author = query.value(4).toString(),
                     .entity = query.value(5).toString(),
                     .id = query.value(6).toString(),
                     .field = query.value(7).toString(),
                     .value = query.value(8).toString(),
                     .signer = query.value(9).toString(),
                     .signature = query.value(10).toByteArray()});
    }
    return list;
}

namespace {

// Keeps operations from elsewhere in `ops`, each once, and what of each
// space and device is now seen.
void storeOperations(const Library &library, std::span<const SpaceOperation> ops) {
    for (const SpaceOperation &op : ops) {
        const QString space = op.space.isEmpty() ? Spaces::personalSpace(library) : op.space;
        QString kind = u"personal"_s;
        for (const auto prefix : {"world"_L1, "published"_L1, "plugin"_L1})
            if (space.startsWith(prefix + u':'))
                kind = prefix;
        ChangeLog::exec(library, "INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, ?, ?, ?, ?)",
                        {space, kind, space, op.author, op.hlc});
        const qint64 spaceNum = ChangeLog::internId(library, space);
        const qint64 deviceNum = ChangeLog::internId(library, op.device);
        const qint64 authorNum = ChangeLog::internId(library, op.author);
        const qint64 idNum = ChangeLog::internId(library, op.id);
        // Kept as it was signed, its signature with it, to be verified again wherever it goes next.
        const bool isSigned = !op.signer.isEmpty() && !op.signature.isEmpty();
        ChangeLog::exec(library,
                        "INSERT OR IGNORE INTO ops (space, device, seq, hlc, author, entity, id, field, value, signer, signature) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        {spaceNum, deviceNum, op.seq, Hlc::normalize(op.hlc), authorNum, op.entity, idNum, op.field, op.value,
                         isSigned ? QVariant(ChangeLog::internId(library, op.signer)) : QVariant(),
                         isSigned ? QVariant(op.signature) : QVariant()});
        ChangeLog::exec(library,
                        "INSERT INTO seen (space, device, seq, hlc) VALUES (?, ?, ?, ?) "
                        "ON CONFLICT(space, device) DO UPDATE SET seq = MAX(seq, excluded.seq), "
                        "hlc = MAX(hlc, excluded.hlc)",
                        {spaceNum, deviceNum, op.seq, Hlc::normalize(op.hlc)});
        Spaces::observeHlc(library, op.hlc);
    }
}

} // namespace

void SpaceReplay::store(const Library &library, std::span<const SpaceOperation> ops) { storeOperations(library, ops); }

bool SpaceReplay::apply(Library &library, std::span<const SpaceOperation> ops) {
    const ChangeLog::Scope guard;
    const Library::Batch batch(library);
    storeOperations(library, ops);
    return replay(library);
}

QList<qint64> SpaceReplay::takePlaces(Library &library, std::span<const SpaceOperation> ops) {
    ChangeLog::afterWrite(library);
    const ChangeLog::Scope guard;
    const Library::Batch batch(library);
    storeOperations(library, ops);
    QList<qint64> changed;
    for (const auto &[book, read] : Replication::placeChanges(library)) {
        ChangeLog::exec(library, "INSERT OR REPLACE INTO progress (book_id, section, fraction, percent, updated) VALUES (?, ?, ?, ?, ?)",
                        {book, read.place.section, read.place.fraction, read.percent, read.updated});
        // The place is the log's already, so it is no write for the log to take in.
        ChangeLog::exec(library, "DELETE FROM log_dirty WHERE entity = 'book' AND local_id = ?", {book});
        changed << book;
    }
    ChangeLog::markLogged(library);
    return changed;
}

bool SpaceReplay::replay(Library &library) {
    const ChangeLog::Scope guard;
    const Library::Batch batch(library);
    const ReducedState state = SpaceReducer::reduce(operations(library));
    materializeBooks(library, state.books);
    materializeCollections(library, state.collections);
    materializeNodes(library, state.nodes);
    materializeFacts(library, state.facts);
    materializeEdges(library, state.edges);
    materializeDays(library, state.days);
    Tenancy::follow(library);
    ChangeLog::markLogged(library);
    return true;
}

QHash<QString, qint64> SpaceReplay::bookFieldHlcs(const Library &library, qint64 bookId) {
    QHash<QString, qint64> hlcs;
    const QString gid = Spaces::bookId(library, bookId);
    if (gid.isEmpty())
        return hlcs;
    const ReducedBook reduced = reducedBookFor(library, gid);
    for (auto it = reduced.fields.cbegin(); it != reduced.fields.cend(); ++it) {
        if (it.key() == "details_at"_L1 || it.key() == "reading_at"_L1 || it.key() == "removed_at"_L1)
            continue;
        if (const qint64 hlc = SpaceReducer::fieldHlc(reduced, it.key()); hlc > 0)
            hlcs.insert(it.key(), hlc);
    }
    return hlcs;
}

qint64 SpaceReplay::entityHlc(const Library &library, const QString &entity, const QString &globalId) {
    if (globalId.isEmpty())
        return 0;
    QSqlQuery query = ChangeLog::query(
        library, "SELECT MAX(hlc) FROM live_ops WHERE entity = ? AND id = (SELECT id FROM log_ids WHERE name = ?)", {entity, globalId});
    return query.next() ? Hlc::normalize(query.value(0).toLongLong()) : 0;
}

qint64 SpaceReplay::fieldHlc(const Library &library, const QString &entity, const QString &globalId, const QString &field) {
    if (globalId.isEmpty())
        return 0;
    if (entity == "book"_L1)
        return SpaceReducer::fieldHlc(reducedBookFor(library, globalId), field);
    QSqlQuery query = ChangeLog::query(
        library, "SELECT MAX(hlc) FROM live_ops WHERE entity = ? AND id = (SELECT id FROM log_ids WHERE name = ?) AND field = ?",
        {entity, globalId, field});
    return query.next() ? Hlc::normalize(query.value(0).toLongLong()) : 0;
}
