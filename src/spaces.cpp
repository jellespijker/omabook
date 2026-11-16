#include "spaces.h"

#include <QCryptographicHash>
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

QString Spaces::personalSpace(const Library &library) {
    QSqlQuery query = library.query("SELECT id FROM spaces WHERE kind = 'personal' ORDER BY id LIMIT 1");
    if (query.next())
        return query.value(0).toString();
    query.finish();
    const QString device = deviceId(library);
    QString space = u"personal:"_s + device;
    const qint64 created = tickHlc(library);
    library.exec("INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, 'personal', 'Personal', ?, ?)",
                 {space, device, created});
    library.exec("INSERT OR IGNORE INTO space_members (space, person, role, added) VALUES (?, ?, 'writer', ?)", {space, device, created});
    return space;
}

QString Spaces::spaceForScope(const Library &library, const QString &scope) {
    QString trimmed = scope.trimmed();
    if (trimmed.isEmpty())
        return personalSpace(library);
    QSqlQuery query = library.query("SELECT id FROM spaces WHERE id = ?", {trimmed});
    if (query.next())
        return trimmed;
    query.finish();
    const QString device = deviceId(library);
    const qint64 created = lastHlc(library);
    library.exec("INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, 'world', ?, ?, ?)",
                 {trimmed, trimmed, device, created});
    library.exec("INSERT OR IGNORE INTO space_members (space, person, role, added) VALUES (?, ?, 'writer', ?)", {trimmed, device, created});
    return trimmed;
}

qint64 Spaces::lastHlc(const Library &library) {
    QSqlQuery query = library.query("SELECT value FROM library_state WHERE key = 'hlc'");
    return query.next() ? Hlc::normalize(query.value(0).toLongLong()) : 0;
}

qint64 Spaces::tickHlc(const Library &library, std::optional<qint64> wallSeconds) {
    Hlc clock(lastHlc(library));
    const qint64 next = wallSeconds ? clock.tick(*wallSeconds) : clock.tick();
    library.exec("INSERT OR REPLACE INTO library_state (key, value) VALUES ('hlc', ?)", {next});
    return next;
}

qint64 Spaces::observeHlc(const Library &library, qint64 remote) {
    Hlc clock(lastHlc(library));
    clock.observe(remote);
    if (clock.last() > 0)
        library.exec("INSERT OR REPLACE INTO library_state (key, value) VALUES ('hlc', ?)", {clock.last()});
    return clock.last();
}

QString Spaces::bookGlobalId(const QStringList &fileHashes, const QStringList &entryKeys, qint64 fallbackId) {
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
    return u"e:book="_s + QString::number(fallbackId);
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
    if (trimmed.startsWith("b:"_L1) || trimmed.startsWith("e:"_L1))
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

QString Spaces::bookId(const Library &library, qint64 localId) {
    if (localId <= 0)
        return {};
    QString existing = globalId(library, u"book"_s, localId);
    // A book with a file keeps the file-hash id it was first given, even as
    // other formats are added or removed later.
    if (existing.startsWith("b:"_L1))
        return existing;
    QStringList hashes;
    for (QSqlQuery query = library.query("SELECT hash FROM formats WHERE book_id = ? AND hash <> ''", {localId}); query.next();)
        hashes << query.value(0).toString();
    if (!hashes.isEmpty()) {
        QString derived = bookGlobalId(hashes, {}, localId);
        if (!existing.isEmpty() && existing != derived)
            library.exec("UPDATE ops SET id = ? WHERE entity = 'book' AND id = ?", {derived, existing});
        setGlobalId(library, u"book"_s, localId, derived);
        return derived;
    }
    if (!existing.isEmpty())
        return existing;
    const std::optional<Book> book = library.book(localId);
    if (!book)
        return {};
    QString derived = bookGlobalId({}, Library::entryKeys(*book), localId);
    setGlobalId(library, u"book"_s, localId, derived);
    return derived;
}

QString Spaces::collectionId(const Library &library, qint64 localId) {
    if (localId <= 0)
        return {};
    QString existing = globalId(library, u"collection"_s, localId);
    for (const Collection &collection : library.collections(true)) {
        if (collection.id == localId) {
            QString derived = collectionGlobalId(collection.path);
            if (!existing.isEmpty() && existing != derived) {
                library.exec("UPDATE ops SET id = ? WHERE entity = 'collection' AND id = ?", {derived, existing});
                library.exec("UPDATE ops SET value = ? WHERE entity = 'collection' AND field = 'parent' AND value = ?",
                             {derived, existing});
            }
            setGlobalId(library, u"collection"_s, localId, derived);
            return derived;
        }
    }
    return existing;
}

QString Spaces::nodeId(const Library &library, qint64 localId) {
    if (localId <= 0)
        return {};
    if (QString existing = globalId(library, u"node"_s, localId); !existing.isEmpty())
        return existing;
    QSqlQuery query = library.query("SELECT scope, kind, name FROM graph_nodes WHERE id = ?", {localId});
    if (!query.next())
        return {};
    QString derived = nodeGlobalId(query.value(0).toString(), query.value(1).toString(), query.value(2).toString());
    query.finish();
    setGlobalId(library, u"node"_s, localId, derived);
    return derived;
}

QString Spaces::edgeId(const Library &library, qint64 localId) {
    if (localId <= 0)
        return {};
    if (QString existing = globalId(library, u"edge"_s, localId); !existing.isEmpty())
        return existing;
    QSqlQuery query = library.query("SELECT source, target, kind FROM graph_edges WHERE id = ?", {localId});
    if (!query.next())
        return {};
    const qint64 source = query.value(0).toLongLong();
    const qint64 target = query.value(1).toLongLong();
    const QString kind = query.value(2).toString();
    query.finish();
    const QString sourceGid = nodeId(library, source);
    const QString targetGid = nodeId(library, target);
    if (sourceGid.isEmpty() || targetGid.isEmpty())
        return {};
    QString derived = edgeGlobalId(sourceGid, targetGid, kind);
    setGlobalId(library, u"edge"_s, localId, derived);
    return derived;
}

QString Spaces::factId(const Library &library, qint64 localId) {
    if (localId <= 0)
        return {};
    if (QString existing = globalId(library, u"fact"_s, localId); !existing.isEmpty())
        return existing;
    QSqlQuery query = library.query("SELECT node_id, kind, value, COALESCE(book_id, 0) FROM graph_facts WHERE id = ?", {localId});
    if (!query.next())
        return {};
    const qint64 node = query.value(0).toLongLong();
    const QString kind = query.value(1).toString();
    const QString value = query.value(2).toString();
    const qint64 book = query.value(3).toLongLong();
    query.finish();
    const QString nodeGid = nodeId(library, node);
    if (nodeGid.isEmpty())
        return {};
    const QString bookGid = book > 0 ? bookId(library, book) : QString();
    QString derived = factGlobalId(nodeGid, kind, value, bookGid);
    setGlobalId(library, u"fact"_s, localId, derived);
    return derived;
}

void Spaces::assignAll(const Library &library) {
    const Library::Batch batch(library);
    (void)personalSpace(library);
    for (QSqlQuery query = library.query("SELECT id FROM books ORDER BY id"); query.next();)
        (void)bookId(library, query.value(0).toLongLong());
    for (const Collection &collection : library.collections(true))
        setGlobalId(library, u"collection"_s, collection.id, collectionGlobalId(collection.path));
    for (QSqlQuery query = library.query("SELECT id, scope FROM graph_nodes ORDER BY id"); query.next();) {
        const QString scope = query.value(1).toString();
        if (!scope.isEmpty())
            (void)spaceForScope(library, scope);
        (void)nodeId(library, query.value(0).toLongLong());
    }
    for (QSqlQuery query = library.query("SELECT id FROM graph_edges ORDER BY id"); query.next();)
        (void)edgeId(library, query.value(0).toLongLong());
    for (QSqlQuery query = library.query("SELECT id FROM graph_facts ORDER BY id"); query.next();)
        (void)factId(library, query.value(0).toLongLong());
}
