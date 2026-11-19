#include "spaces.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <initializer_list>

#include "hlc.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString hashParts(std::initializer_list<QString> parts) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    bool first = true;
    for (const QString &part : parts) {
        if (!first)
            hash.addData(QByteArrayView("\x1f", 1));
        first = false;
        const QByteArray utf8 = part.toUtf8();
        hash.addData(utf8);
    }
    return QString::fromLatin1(hash.result().toHex().left(32));
}

} // namespace

QString Spaces::deviceId(const Library &library) {
    if (QString existing = globalId(library, u"device"_s, 0); !existing.isEmpty())
        return existing;
    QString created = QUuid::createUuid().toString(QUuid::Id128);
    setGlobalId(library, u"device"_s, 0, created);
    return created;
}

void Spaces::ensurePersonalSpace(const Library &library) {
    QSqlQuery query = library.query("SELECT 1 FROM spaces WHERE kind = 'personal' LIMIT 1");
    if (query.next())
        return;
    query.finish();
    const QString device = deviceId(library);
    const QString space = u"personal:"_s + device;
    const qint64 created = tickHlc(library);
    library.exec("INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, 'personal', 'Personal', ?, ?)",
                 {space, device, created});
    library.exec("INSERT OR IGNORE INTO space_members (space, person, role, added) VALUES (?, ?, 'writer', ?)", {space, device, created});
}

QString Spaces::personalSpace(const Library &library) {
    ensurePersonalSpace(library);
    QSqlQuery query = library.query("SELECT id FROM spaces WHERE kind = 'personal' ORDER BY id LIMIT 1");
    return query.next() ? query.value(0).toString() : (u"personal:"_s + deviceId(library));
}

void Spaces::pair(const Library &library, const QString &theirSpace, const QString &theirDevice) {
    if (!theirSpace.startsWith("personal:"_L1) || theirDevice.isEmpty())
        return;
    const qint64 created = lastHlc(library);
    library.exec("INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, 'personal', 'Personal', ?, ?)",
                 {theirSpace, theirDevice, created});
    // Both computers are members of both spaces: each holds the other's
    // operations too, so compaction drops of either only what both have seen.
    for (const QString &space : {personalSpace(library), theirSpace, u"personal:"_s + deviceId(library)})
        for (const QString &device : {deviceId(library), theirDevice})
            library.exec("INSERT OR IGNORE INTO space_members (space, person, role, added) VALUES (?, ?, 'writer', ?)",
                         {space, device, created});
}

void Spaces::ensureSpaceForScope(const Library &library, const QString &scope) {
    const QString trimmed = scope.trimmed();
    if (trimmed.isEmpty()) {
        ensurePersonalSpace(library);
        return;
    }
    QSqlQuery query = library.query("SELECT 1 FROM spaces WHERE id = ?", {trimmed});
    if (query.next())
        return;
    query.finish();
    const QString device = deviceId(library);
    const qint64 created = lastHlc(library);
    library.exec("INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, 'world', ?, ?, ?)",
                 {trimmed, trimmed, device, created});
    library.exec("INSERT OR IGNORE INTO space_members (space, person, role, added) VALUES (?, ?, 'writer', ?)", {trimmed, device, created});
}

QString Spaces::spaceForScope(const Library &library, const QString &scope) {
    QString trimmed = scope.trimmed();
    if (trimmed.isEmpty())
        return personalSpace(library);
    ensureSpaceForScope(library, trimmed);
    return trimmed;
}

void Spaces::ensurePublishedSpace(const Library &library, const QString &space, const QString &name, const QString &owner,
                                  const QString &ownerName) {
    if (!space.startsWith("published:"_L1) || owner.isEmpty())
        return;
    const qint64 created = lastHlc(library);
    library.exec("INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, 'published', ?, ?, ?)",
                 {space, name, owner, created});
    library.exec("INSERT INTO space_members (space, person, role, name, added) VALUES (?, ?, 'writer', ?, ?) "
                 "ON CONFLICT(space, person) DO UPDATE SET name = excluded.name",
                 {space, owner, ownerName, created});
}

qint64 Spaces::lastHlc(const Library &library) {
    QSqlQuery query = library.query("SELECT value FROM library_state WHERE key = 'hlc'");
    return query.next() ? Hlc::normalize(query.value(0).toLongLong()) : 0;
}

qint64 Spaces::tickHlc(const Library &library, std::optional<qint64> wallSeconds) {
    Hlc clock(lastHlc(library));
    const qint64 wall = wallSeconds ? *wallSeconds : (QDateTime::currentSecsSinceEpoch() + clockOffset(library));
    const qint64 next = clock.tick(wall);
    library.exec("INSERT OR REPLACE INTO library_state (key, value) VALUES ('hlc', ?)", {next});
    return next;
}

void Spaces::observeHlc(const Library &library, qint64 remote) {
    Hlc clock(lastHlc(library));
    clock.observe(remote);
    if (clock.last() > 0)
        library.exec("INSERT OR REPLACE INTO library_state (key, value) VALUES ('hlc', ?)", {clock.last()});
}

void Spaces::setClockOffset(const Library &library, qint64 offsetSeconds) {
    library.exec("INSERT OR REPLACE INTO library_state (key, value) VALUES ('clock_offset', ?)", {offsetSeconds});
}

qint64 Spaces::clockOffset(const Library &library) {
    QSqlQuery query = library.query("SELECT value FROM library_state WHERE key = 'clock_offset'");
    return query.next() ? query.value(0).toLongLong() : 0;
}

qint64 Spaces::wallSeconds(const Library &library) {
    return std::max(QDateTime::currentSecsSinceEpoch() + clockOffset(library), Hlc::seconds(lastHlc(library)));
}

QString Spaces::bookGlobalId(const QStringList &fileHashes, const QStringList &entryKeys) {
    QStringList hashes;
    for (const QString &hash : fileHashes)
        if (!hash.trimmed().isEmpty())
            hashes << hash.trimmed().toLower();
    if (!hashes.isEmpty()) {
        std::ranges::sort(hashes);
        return u"b:"_s + hashes.first();
    }
    if (!entryKeys.isEmpty() && !entryKeys.first().trimmed().isEmpty())
        return u"e:"_s + entryKeys.first().trimmed();
    return u"u:"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString Spaces::collectionGlobalId(const QString &path) { return u"c:"_s + path.trimmed(); }

QString Spaces::nodeGlobalId(const QString &scope, const QString &kind, const QString &name) {
    return u"n:"_s + hashParts({scope.trimmed(), kind.simplified().toLower(), name.simplified().toCaseFolded()});
}

QString Spaces::edgeGlobalId(const QString &sourceGlobalId, const QString &targetGlobalId, const QString &kind) {
    return u"r:"_s + hashParts({sourceGlobalId.trimmed(), targetGlobalId.trimmed(), kind.simplified().toLower()});
}

QString Spaces::factGlobalId(const QString &nodeGlobalId, const QString &kind, const QString &value, const QString &bookGlobalId) {
    return u"f:"_s + hashParts({nodeGlobalId.trimmed(), kind.simplified().toLower(), value.trimmed(), bookGlobalId.trimmed()});
}

QString Spaces::removalGlobalId(const QString &removalKey) {
    QString trimmed = removalKey.trimmed();
    if (trimmed.startsWith("b:"_L1) || trimmed.startsWith("e:"_L1) || trimmed.startsWith("u:"_L1))
        return trimmed;
    return (trimmed.startsWith("entry:"_L1) ? u"e:"_s : u"b:"_s) + trimmed;
}

QString Spaces::globalId(const Library &library, const QString &entity, qint64 localId) {
    QSqlQuery query = library.query("SELECT global_id FROM global_ids WHERE entity = ? AND local_id = ?", {entity, localId});
    return query.next() ? query.value(0).toString() : QString();
}

qint64 Spaces::localId(const Library &library, const QString &entity, const QString &globalId) {
    QSqlQuery query = library.query("SELECT local_id FROM global_ids WHERE entity = ? AND global_id = ?", {entity, globalId});
    return query.next() ? query.value(0).toLongLong() : 0;
}

void Spaces::setGlobalId(const Library &library, const QString &entity, qint64 localId, const QString &globalId) {
    if (globalId.isEmpty())
        return;
    library.exec("DELETE FROM global_ids WHERE entity = ? AND global_id = ? AND local_id <> ?", {entity, globalId, localId});
    library.exec("INSERT OR REPLACE INTO global_ids (entity, local_id, global_id) VALUES (?, ?, ?)", {entity, localId, globalId});
}

void Spaces::ensureBookId(const Library &library, qint64 localId) {
    if (localId <= 0 || !globalId(library, u"book"_s, localId).isEmpty())
        return;
    QStringList hashes;
    for (QSqlQuery query = library.query("SELECT hash FROM formats WHERE book_id = ? AND hash <> ''", {localId}); query.next();)
        hashes << query.value(0).toString();
    if (!hashes.isEmpty()) {
        setGlobalId(library, u"book"_s, localId, bookGlobalId(hashes, {}));
        return;
    }
    const std::optional<Book> book = library.book(localId);
    if (!book)
        return;
    setGlobalId(library, u"book"_s, localId, bookGlobalId({}, Library::entryKeys(*book)));
}

QString Spaces::bookId(const Library &library, qint64 localId) {
    ensureBookId(library, localId);
    return globalId(library, u"book"_s, localId);
}

void Spaces::ensureCollectionId(const Library &library, qint64 localId) {
    if (localId <= 0 || !globalId(library, u"collection"_s, localId).isEmpty())
        return;
    for (const Collection &collection : library.collections(true)) {
        if (collection.id == localId) {
            setGlobalId(library, u"collection"_s, localId, collectionGlobalId(collection.path));
            return;
        }
    }
}

QString Spaces::collectionId(const Library &library, qint64 localId) {
    ensureCollectionId(library, localId);
    return globalId(library, u"collection"_s, localId);
}

void Spaces::ensureNodeId(const Library &library, qint64 localId) {
    if (localId <= 0 || !globalId(library, u"node"_s, localId).isEmpty())
        return;
    QSqlQuery query = library.query("SELECT scope, kind, name FROM graph_nodes WHERE id = ?", {localId});
    if (!query.next())
        return;
    const QString derived = nodeGlobalId(query.value(0).toString(), query.value(1).toString(), query.value(2).toString());
    query.finish();
    setGlobalId(library, u"node"_s, localId, derived);
}

QString Spaces::nodeId(const Library &library, qint64 localId) {
    ensureNodeId(library, localId);
    return globalId(library, u"node"_s, localId);
}

void Spaces::ensureEdgeId(const Library &library, qint64 localId) {
    if (localId <= 0 || !globalId(library, u"edge"_s, localId).isEmpty())
        return;
    QSqlQuery query = library.query("SELECT source, target, kind FROM graph_edges WHERE id = ?", {localId});
    if (!query.next())
        return;
    const qint64 source = query.value(0).toLongLong();
    const qint64 target = query.value(1).toLongLong();
    const QString kind = query.value(2).toString();
    query.finish();
    const QString sourceGid = nodeId(library, source);
    const QString targetGid = nodeId(library, target);
    if (!sourceGid.isEmpty() && !targetGid.isEmpty())
        setGlobalId(library, u"edge"_s, localId, edgeGlobalId(sourceGid, targetGid, kind));
}

QString Spaces::edgeId(const Library &library, qint64 localId) {
    ensureEdgeId(library, localId);
    return globalId(library, u"edge"_s, localId);
}

void Spaces::ensureFactId(const Library &library, qint64 localId) {
    if (localId <= 0 || !globalId(library, u"fact"_s, localId).isEmpty())
        return;
    QSqlQuery query = library.query("SELECT node_id, kind, value, COALESCE(book_id, 0) FROM graph_facts WHERE id = ?", {localId});
    if (!query.next())
        return;
    const qint64 node = query.value(0).toLongLong();
    const QString kind = query.value(1).toString();
    const QString value = query.value(2).toString();
    const qint64 book = query.value(3).toLongLong();
    query.finish();
    const QString nodeGid = nodeId(library, node);
    if (!nodeGid.isEmpty())
        setGlobalId(library, u"fact"_s, localId, factGlobalId(nodeGid, kind, value, book > 0 ? bookId(library, book) : QString()));
}

QString Spaces::factId(const Library &library, qint64 localId) {
    ensureFactId(library, localId);
    return globalId(library, u"fact"_s, localId);
}

void Spaces::assignAll(const Library &library) {
    const Library::Batch batch(library);
    ensurePersonalSpace(library);
    for (QSqlQuery query = library.query("SELECT id FROM books ORDER BY id"); query.next();)
        ensureBookId(library, query.value(0).toLongLong());
    for (const Collection &collection : library.collections(true))
        ensureCollectionId(library, collection.id);
    for (QSqlQuery query = library.query("SELECT id, scope FROM graph_nodes ORDER BY id"); query.next();) {
        const QString scope = query.value(1).toString();
        if (!scope.isEmpty())
            ensureSpaceForScope(library, scope);
        ensureNodeId(library, query.value(0).toLongLong());
    }
    for (QSqlQuery query = library.query("SELECT id FROM graph_edges ORDER BY id"); query.next();)
        ensureEdgeId(library, query.value(0).toLongLong());
    for (QSqlQuery query = library.query("SELECT id FROM graph_facts ORDER BY id"); query.next();)
        ensureFactId(library, query.value(0).toLongLong());
}
