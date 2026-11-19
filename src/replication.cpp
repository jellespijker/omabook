#include "replication.h"

#include <QHash>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <tuple>

#include "changelog.h"
#include "hlc.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// The operations that travel by the log, in SQL; Replication::travels says the same of one.
constexpr auto travelling = "o.entity = 'book' AND o.field IN ('progress', 'same_as')";

[[nodiscard]] QString standsFor(const QHash<QString, QString> &canonical, const QString &gid) { return canonical.value(gid, gid); }

// Of a book's linked group, every id now stands for the smaller of the two
// roots, as SpaceReducer::sameAs would answer with the new link among the rest.
void join(QHash<QString, QString> &canonical, const QString &one, const QString &other) {
    const QString first = standsFor(canonical, one);
    const QString second = standsFor(canonical, other);
    const QString root = std::min(first, second);
    const QString joined = std::max(first, second);
    for (QString &stands : canonical)
        if (stands == joined)
            stands = root;
    for (const QString &id : {one, other, first, second})
        canonical.insert(id, root);
}

[[nodiscard]] QHash<QString, qint64> booksByEntryKey(const Library &library) {
    QHash<QString, qint64> books;
    for (const Book &book : library.books())
        for (const QString &key : Library::entryKeys(book))
            books.insert(key, book.id);
    return books;
}

[[nodiscard]] Progress placeOf(const ReducedBook &book) {
    const QJsonObject value = QJsonDocument::fromJson(book.fields.value(u"progress"_s).value.toUtf8()).object();
    return {.section = value.value("section").toInt(),
            .fraction = value.value("fraction").toDouble(),
            .percent = value.value("percent").toDouble(),
            .updated = Hlc::seconds(SpaceReducer::fieldHlc(book, u"progress"_s))};
}

} // namespace

bool Replication::travels(const SpaceOperation &op) {
    return op.entity == "book"_L1 && (op.field == "progress"_L1 || op.field == "same_as"_L1);
}

QList<SpaceOperation> Replication::operations(const Library &library) {
    QList<SpaceOperation> list;
    for (QSqlQuery query =
             ChangeLog::query(library, QString("SELECT s.name, d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value "
                                               "FROM ops AS o JOIN log_ids AS s ON s.id = o.space "
                                               "JOIN log_ids AS d ON d.id = o.device JOIN log_ids AS a ON a.id = o.author "
                                               "JOIN log_ids AS i ON i.id = o.id WHERE %1 ORDER BY s.name, d.name, o.seq")
                                           .arg(travelling));
         query.next();)
        list.append({.space = query.value(0).toString(),
                     .device = query.value(1).toString(),
                     .seq = query.value(2).toLongLong(),
                     .hlc = query.value(3).toLongLong(),
                     .author = query.value(4).toString(),
                     .entity = query.value(5).toString(),
                     .id = query.value(6).toString(),
                     .field = query.value(7).toString(),
                     .value = query.value(8).toString()});
    return list;
}

Replication::Vector Replication::vector(const Library &library) {
    Vector known;
    for (QSqlQuery query = ChangeLog::query(library, QString("SELECT s.name, d.name, MAX(o.seq) FROM ops AS o "
                                                             "JOIN log_ids AS s ON s.id = o.space JOIN log_ids AS d ON d.id = o.device "
                                                             "WHERE %1 GROUP BY o.space, o.device")
                                                         .arg(travelling));
         query.next();)
        known[query.value(0).toString()][query.value(1).toString()] = query.value(2).toLongLong();
    return known;
}

QList<SpaceOperation> Replication::since(const Library &library, const Vector &known) {
    QList<SpaceOperation> lacking = operations(library);
    lacking.removeIf([&known](const SpaceOperation &op) { return op.seq <= known.value(op.space).value(op.device); });
    return lacking;
}

QJsonObject Replication::toJson(const Vector &vector) {
    QJsonObject object;
    for (auto space = vector.cbegin(); space != vector.cend(); ++space) {
        QJsonObject devices;
        for (auto device = space->cbegin(); device != space->cend(); ++device)
            devices.insert(device.key(), device.value());
        object.insert(space.key(), devices);
    }
    return object;
}

Replication::Vector Replication::vectorFrom(const QJsonObject &object) {
    Vector vector;
    for (auto space = object.begin(); space != object.end(); ++space) {
        const QJsonObject devices = space.value().toObject();
        for (auto device = devices.begin(); device != devices.end(); ++device)
            vector[space.key()][device.key()] = device.value().toInteger();
    }
    return vector;
}

QJsonArray Replication::toJson(std::span<const SpaceOperation> ops) {
    QJsonArray array;
    for (const SpaceOperation &op : ops)
        array << QJsonArray{op.space, op.device, op.seq, op.hlc, op.author, op.entity, op.id, op.field, op.value};
    return array;
}

QList<SpaceOperation> Replication::operationsFrom(const QJsonArray &array) {
    QList<SpaceOperation> ops;
    for (const auto &value : array) {
        const QJsonArray op = value.toArray();
        const SpaceOperation taken{.space = op.at(0).toString(),
                                   .device = op.at(1).toString(),
                                   .seq = op.at(2).toInteger(),
                                   .hlc = op.at(3).toInteger(),
                                   .author = op.at(4).toString(),
                                   .entity = op.at(5).toString(),
                                   .id = op.at(6).toString(),
                                   .field = op.at(7).toString(),
                                   .value = op.at(8).toString()};
        if (op.size() == 9 && travels(taken) && !taken.space.isEmpty() && !taken.device.isEmpty() && taken.seq > 0 && !taken.id.isEmpty())
            ops << taken;
    }
    return ops;
}

QJsonArray Replication::toJson(const QList<BookNames> &books) {
    QJsonArray array;
    for (const BookNames &book : books)
        array << QJsonObject{
            {"gid", book.gid}, {"hashes", QJsonArray::fromStringList(book.hashes)}, {"keys", QJsonArray::fromStringList(book.keys)}};
    return array;
}

QList<Replication::BookNames> Replication::booksFrom(const QJsonArray &array) {
    QList<BookNames> books;
    for (const auto &value : array) {
        const QJsonObject book = value.toObject();
        const auto strings = [&book](const char *key) {
            QStringList list;
            for (const auto &item : book.value(QLatin1StringView(key)).toArray())
                list << item.toString();
            return list;
        };
        if (const QString gid = book.value("gid").toString(); !gid.isEmpty())
            books << BookNames{.gid = gid, .hashes = strings("hashes"), .keys = strings("keys")};
    }
    return books;
}

QList<Replication::BookNames> Replication::unlinked(const Library &library, const QSet<QString> &theirGids) {
    const QHash<QString, QString> canonical = SpaceReducer::sameAs(operations(library));
    QSet<QString> known;
    for (const QString &gid : theirGids)
        known.insert(standsFor(canonical, gid));
    QList<BookNames> books;
    for (const Book &book : library.books()) {
        const QString gid = Spaces::bookId(library, book.id);
        if (gid.isEmpty() || known.contains(standsFor(canonical, gid)))
            continue;
        QStringList hashes;
        for (const FormatFile &file : library.formatFiles(book.id))
            hashes << file.hash;
        books << BookNames{.gid = gid, .hashes = hashes, .keys = hashes.isEmpty() ? Library::entryKeys(book) : QStringList()};
    }
    return books;
}

int Replication::link(const Library &library, const QList<BookNames> &theirs) {
    const Library::Batch batch(library);
    QHash<QString, QString> canonical = SpaceReducer::sameAs(operations(library));
    QHash<QString, qint64> byKey;
    int linked = 0;
    for (const BookNames &their : theirs) {
        // Today's rules, as the manifest sync matches a book: a file with
        // the same content, and for a book without a file its entry keys.
        qint64 mine = 0;
        for (const QString &hash : their.hashes) {
            mine = library.bookWithHash(hash);
            if (mine > 0)
                break;
        }
        if (mine == 0 && their.hashes.isEmpty()) {
            if (byKey.isEmpty())
                byKey = booksByEntryKey(library);
            const auto found = std::ranges::find_if(their.keys, [&byKey](const QString &key) { return byKey.contains(key); });
            mine = found != their.keys.cend() ? byKey.value(*found) : 0;
        }
        const QString myGid = mine > 0 ? Spaces::bookId(library, mine) : QString();
        if (myGid.isEmpty() || standsFor(canonical, myGid) == standsFor(canonical, their.gid))
            continue;
        ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                    .entity = u"book"_s,
                                    .id = std::max(myGid, their.gid),
                                    .field = u"same_as"_s,
                                    .value = std::min(myGid, their.gid),
                                    .hlc = std::nullopt});
        join(canonical, myGid, their.gid);
        ++linked;
    }
    return linked;
}

QList<Replication::PlaceChange> Replication::placeChanges(const Library &library, std::span<const SpaceOperation> incoming) {
    QList<SpaceOperation> ops = operations(library);
    ops.append(QList<SpaceOperation>(incoming.begin(), incoming.end()));
    const ReducedState state = SpaceReducer::reduce(ops);
    const QHash<QString, QString> canonical = SpaceReducer::sameAs(ops);
    QList<PlaceChange> changes;
    for (QSqlQuery query =
             ChangeLog::query(library, "SELECT b.id, g.global_id, p.section, p.fraction, p.percent, p.updated FROM books AS b "
                                       "JOIN global_ids AS g ON g.entity = 'book' AND g.local_id = b.id "
                                       "LEFT JOIN progress AS p ON p.book_id = b.id");
         query.next();) {
        const auto book = state.books.constFind(standsFor(canonical, query.value(1).toString()));
        if (book == state.books.cend() || book->fields.value(u"progress"_s).value.isEmpty())
            continue;
        const Progress place = placeOf(*book);
        const auto row =
            std::tuple{query.value(2).toInt(), query.value(3).toDouble(), query.value(4).toDouble(), query.value(5).toLongLong()};
        if (query.value(2).isNull() || row != std::tuple{place.section, place.fraction, place.percent, place.updated})
            changes << PlaceChange{.book = query.value(0).toLongLong(), .place = place};
    }
    return changes;
}
