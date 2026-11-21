#include "replication.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include "changelog.h"
#include "hlc.h"
#include "opkinds.h"
#include "plugindata.h"
#include "signedops.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// How a query reads the log: all of it, or by ranges of a space and device
// alone, where what an operation is of must not make it read by thing instead.
enum class Reading : std::uint8_t { All, RangesOnly };

// The spaces whose operations travel by the log, in SQL, of `s`. A space this
// computer subscribes to, and a shared one, travel to one's other computers
// sealed beside the log (SpaceCarry, #51), never in it.
[[nodiscard]] QString travellingSpace() {
    return u"s.name NOT LIKE 'shared:%' AND s.name NOT IN (SELECT space FROM space_members WHERE role = 'reader' "
           "AND person = (SELECT global_id FROM global_ids WHERE entity = 'device' AND local_id = 0))"_s;
}

// Whether a query reads the log for what is taken in here, or for what is
// held and sent on: what is kept of a kind this Omabook doesn't know yet
// (KeptOps, #118) is held and sent on, never taken in.
enum class Use : std::uint8_t { Taking, Sending };

// An operation kept of a kind not known here, in SQL, of `o`.
constexpr auto kept = "EXISTS (SELECT 1 FROM kept_ops AS k WHERE k.space = o.space AND k.device = o.device AND k.seq = o.seq)";

// The operations that travel, in SQL, of `o` in a space `s`; Replication::travels says the same of one.
// Of a book the fields OpKinds says, every field of a collection, and
// everything of the general graph (the personal space's) and of a story's
// world (its own space, `world:…`), which goes where its story goes (#25),
// the bookmarks and highlights (#66), and a plugin's data, all of its own
// space (#76).
[[nodiscard]] QString travelling(Reading reading = Reading::All, Use use = Use::Taking) {
    QStringList fields;
    for (const QLatin1StringView field : OpKinds::travellingBookFields())
        fields << u"'"_s + field + u"'"_s;
    // A unary plus keeps SQLite from looking the thing up by its index.
    const QString entity = reading == Reading::RangesOnly ? u"+o.entity"_s : u"o.entity"_s;
    const QString kinds = u"(("_s + entity + u" = 'book' AND (o.field IN ("_s + fields.join(u", "_s) + u") OR o.field LIKE 'tag:%')) OR "_s
                          + entity + u" = 'collection' OR ("_s + entity
                          + u" IN ('node', 'fact', 'edge', 'reading_log', 'subscription', 'annotation', 'goal', 'private') "
                            "AND (s.name LIKE 'personal:%' OR s.name LIKE 'world:%')) OR s.name LIKE 'plugin:%')"_s;
    const QString which = use == Use::Sending ? u"("_s + kinds + u" OR "_s + QLatin1StringView(kept) + u")"_s
                                              : kinds + u" AND NOT "_s + QLatin1StringView(kept);
    return which + u" AND "_s + travellingSpace();
}

// The names of an operation's space, device, author, thing and signer, for
// a query of the log whose operations are `o`.
constexpr auto namesJoined =
    "JOIN log_ids AS s ON s.id = o.space JOIN log_ids AS d ON d.id = o.device JOIN log_ids AS a ON a.id = o.author "
    "JOIN log_ids AS i ON i.id = o.id LEFT JOIN log_ids AS g ON g.id = o.signer";

// Every space and device whose operations travel, with its mark (0 for
// none): each operation written or taken in is recorded in `seen`, and of
// each the snapshot holds there is a mark (Compaction), so the log itself is
// not read to find them.
[[nodiscard]] QString travellingPairs() {
    return u"SELECT k.space, k.device, COALESCE(m.seq, 0) AS mark FROM (SELECT space, device FROM seen "
           "UNION SELECT space, device FROM snapshot_marks) AS k JOIN log_ids AS s ON s.id = k.space "
           "LEFT JOIN snapshot_marks AS m ON m.space = k.space AND m.device = k.device WHERE "_s
           + travellingSpace();
}

// The operations `sql` answers, each by its eleven parts in the order of SpaceOperation.
[[nodiscard]] QList<SpaceOperation> operationsOf(const Library &library, const QString &sql, const QVariantList &values = {}) {
    QList<SpaceOperation> list;
    for (QSqlQuery query = ChangeLog::query(library, sql, values); query.next();)
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
    return list;
}

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

[[nodiscard]] QString text(const ReducedBook &book, QLatin1StringView field) { return book.fields.value(QString(field)).value; }

// When each of a book's details changed, as a manifest says it (field_hlc):
// none when no time is known of them.
[[nodiscard]] QJsonObject fieldTimes(const ReducedBook &book, qint64 details) {
    if (details == 0)
        return {};
    const auto at = [&](QLatin1StringView field) {
        const qint64 hlc = SpaceReducer::fieldHlc(book, QString(field));
        return hlc > 0 ? hlc : details;
    };
    return {{"title", at("title"_L1)},
            {"authors", at("authors"_L1)},
            {"series", std::max(at("series"_L1), at("series_index"_L1))},
            {"publisher", at("publisher"_L1)},
            {"language", at("language"_L1)},
            {"isbn", at("isbn"_L1)},
            {"published", at("published"_L1)},
            {"description", at("description"_L1)},
            {"tags", at("tags"_L1)},
            {"cover", std::max(at("has_cover"_L1), at("cover_hash"_L1))}};
}

[[nodiscard]] Progress placeOf(const ReducedBook &book) {
    const QJsonObject value = QJsonDocument::fromJson(book.fields.value(u"progress"_s).value.toUtf8()).object();
    return {.place = BookPlace::fromJson(value),
            .percent = value.value("percent").toDouble(),
            .updated = Hlc::seconds(SpaceReducer::fieldHlc(book, u"progress"_s))};
}

} // namespace

bool Replication::travels(const SpaceOperation &op) {
    if (op.space.startsWith("shared:"_L1))
        return false;
    if (PluginData::isPluginSpace(op.space))
        return true;
    if (op.entity == "collection"_L1)
        return true;
    if (op.entity == "node"_L1 || op.entity == "fact"_L1 || op.entity == "edge"_L1 || op.entity == "reading_log"_L1
        || op.entity == "subscription"_L1 || op.entity == "annotation"_L1 || op.entity == "goal"_L1 || op.entity == "private"_L1)
        return op.space.startsWith("personal:"_L1) || op.space.startsWith("world:"_L1);
    return op.entity == "book"_L1 && OpKinds::travelsOfABook(op.field);
}

QJsonObject Replication::bookView(const ReducedBook &book) {
    const qint64 details = SpaceReducer::detailsHlc(book);
    return {{"title", text(book, "title"_L1)},
            {"authors", QJsonArray::fromStringList(BookMeta::splitAuthors(text(book, "authors"_L1)))},
            {"series", text(book, "series"_L1)},
            {"series_index", text(book, "series_index"_L1).toDouble()},
            {"publisher", text(book, "publisher"_L1)},
            {"language", text(book, "language"_L1)},
            {"isbn", text(book, "isbn"_L1)},
            {"published", text(book, "published"_L1)},
            {"description", text(book, "description"_L1)},
            {"tags", QJsonArray::fromStringList(BookMeta::splitTags(text(book, "tags"_L1)))},
            {"identifiers", QJsonDocument::fromJson(text(book, "identifiers"_L1).toUtf8()).object()},
            {"cover_hash", text(book, "cover_hash"_L1)},
            {"shelf", text(book, "shelf"_L1)},
            {"rating", text(book, "rating"_L1).toInt()},
            {"finished", text(book, "finished"_L1)},
            {"paper", text(book, "paper"_L1) == "1"_L1},
            {"details_at", Hlc::seconds(details)},
            {"details_hlc", details},
            {"reading_hlc", SpaceReducer::readingHlc(book)},
            {"field_hlc", fieldTimes(book, details)}};
}

QList<SpaceOperation> Replication::operations(const Library &library) {
    return operationsOf(library, QString("SELECT s.name, d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value, "
                                         "g.name, o.signature FROM space_ops AS o %1 WHERE %2 ORDER BY s.name, d.name, o.seq")
                                     .arg(QString::fromLatin1(namesJoined), travelling()));
}

Replication::Vector Replication::vector(const Library &library) {
    Vector known;
    // Of each space and device, the last that travels after its mark, else,
    // of one the snapshot holds any that travels of, at least its mark: what
    // the snapshot leaves out of that is held in its place. Each is the
    // newest of a range of the log, read from its end (#103).
    for (QSqlQuery query = ChangeLog::query(
             library, QString("SELECT name, device, last FROM (SELECT s.name, d.name AS device, COALESCE("
                              "(SELECT o.seq FROM ops AS o WHERE o.space = p.space AND o.device = p.device AND o.seq > p.mark AND %1 "
                              "ORDER BY o.seq DESC LIMIT 1), "
                              "(SELECT MAX(o.seq, p.mark) FROM snapshot_ops AS o WHERE o.space = p.space AND o.device = p.device AND %1 "
                              "ORDER BY o.seq DESC LIMIT 1)) AS last FROM (%2) AS p "
                              "JOIN log_ids AS s ON s.id = p.space JOIN log_ids AS d ON d.id = p.device) WHERE last IS NOT NULL")
                          .arg(travelling(Reading::RangesOnly, Use::Sending), travellingPairs()));
         query.next();)
        known[query.value(0).toString()][query.value(1).toString()] = query.value(2).toLongLong();
    return known;
}

Replication::Vector Replication::annotationVector(const Library &library) {
    Vector known;
    for (QSqlQuery query = ChangeLog::query(library, QString("SELECT s.name, d.name, MAX(o.seq) FROM space_ops AS o "
                                                             "JOIN log_ids AS s ON s.id = o.space JOIN log_ids AS d ON d.id = o.device "
                                                             "WHERE %1 AND o.entity = 'annotation' GROUP BY o.space, o.device")
                                                         .arg(travelling(Reading::All, Use::Sending)));
         query.next();)
        known[query.value(0).toString()][query.value(1).toString()] = query.value(2).toLongLong();
    return known;
}

QList<SpaceOperation> Replication::since(const Library &library, const Vector &known) { return since(library, known, known); }

QList<SpaceOperation> Replication::since(const Library &library, const Vector &known, const Vector &annotations) {
    // Of each space and device here, what the other side holds, and of the
    // bookmarks and highlights apart: only the operations after the less of
    // the two are read, after the mark from `ops`, and before it from the
    // snapshot only for a side behind the mark (#103).
    QStringList rows;
    QVariantList values;
    std::set<std::pair<QString, QString>> pairs;
    for (const Vector *vector : {&known, &annotations})
        for (auto space = vector->cbegin(); space != vector->cend(); ++space)
            for (auto device = space->cbegin(); device != space->cend(); ++device)
                pairs.insert({space.key(), device.key()});
    for (const auto &[space, device] : pairs) {
        rows << u"(?, ?, ?, ?)"_s;
        values << space << device << known.value(space).value(device) << annotations.value(space).value(device);
    }
    const QString held = rows.isEmpty() ? u"SELECT NULL, NULL, 0, 0 WHERE 0"_s : u"VALUES "_s + rows.join(u", "_s);
    const QString columns = u"x.space, x.device, x.seq, x.hlc, x.author, x.entity, x.id, x.field, x.value, x.signer, x.signature, "
                            "p.seq AS peer_seq, p.notes AS peer_notes"_s;
    return operationsOf(
        library,
        QString("WITH held(space, device, seq, notes) AS (%1), "
                "peer AS (SELECT sn.id AS space, dn.id AS device, h.seq, h.notes FROM held AS h "
                "JOIN log_ids AS sn ON sn.name = h.space JOIN log_ids AS dn ON dn.name = h.device), "
                "pairs AS (SELECT q.space, q.device, q.mark, COALESCE(v.seq, 0) AS seq, COALESCE(v.notes, 0) AS notes, "
                "MIN(COALESCE(v.seq, 0), COALESCE(v.notes, 0)) AS low FROM (%2) AS q "
                "LEFT JOIN peer AS v ON v.space = q.space AND v.device = q.device) "
                "SELECT s.name, d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value, g.name, o.signature FROM ("
                "SELECT %3 FROM pairs AS p CROSS JOIN ops AS x ON x.space = p.space AND x.device = p.device AND x.seq > MAX(p.mark, p.low) "
                "UNION ALL SELECT %3 FROM pairs AS p CROSS JOIN snapshot_ops AS x ON x.space = p.space AND x.device = p.device "
                "AND x.seq > p.low WHERE p.low < p.mark) AS o %4 "
                "WHERE %5 AND o.seq > (CASE WHEN o.entity = 'annotation' THEN o.peer_notes ELSE o.peer_seq END) "
                "ORDER BY s.name, d.name, o.seq")
            .arg(held, travellingPairs(), columns, QString::fromLatin1(namesJoined), travelling(Reading::RangesOnly, Use::Sending)),
        values);
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

QJsonArray Replication::toJson(std::span<const SpaceOperation> ops, bool signatures) {
    QJsonArray array;
    for (const SpaceOperation &op : ops) {
        QJsonArray parts{op.space, op.device, op.seq, op.hlc, op.author, op.entity, op.id, op.field, op.value};
        if (signatures)
            parts << op.signer
                  << QString::fromLatin1(op.signature.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        array << parts;
    }
    return array;
}

QList<SpaceOperation> Replication::operationsFrom(const QJsonArray &array) {
    QList<SpaceOperation> ops = anyOperationsFrom(array);
    ops.removeIf([](const SpaceOperation &op) { return !travels(op) && (op.space.startsWith("shared:"_L1) || OpKinds::known(op)); });
    return ops;
}

QList<SpaceOperation> Replication::anyOperationsFrom(const QJsonArray &array) {
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
                                   .value = op.at(8).toString(),
                                   .signer = op.at(9).toString(),
                                   .signature = QByteArray::fromBase64(op.at(10).toString().toLatin1(), QByteArray::Base64UrlEncoding)};
        if ((op.size() == 9 || op.size() == 11) && !taken.space.isEmpty() && !taken.device.isEmpty() && taken.seq > 0
            && !taken.id.isEmpty())
            ops << taken;
    }
    return ops;
}

Replication::Admitted Replication::admit(const Library &library, const QList<SpaceOperation> &incoming, const QString &peer) {
    Admitted admitted;
    std::optional<bool> ownComputer;
    for (const SpaceOperation &op : incoming) {
        bool taken = false;
        if (!op.signer.isEmpty() || !op.signature.isEmpty()) {
            taken = SignedOps::verify(op);
        } else {
            if (!ownComputer)
                ownComputer = Spaces::isOwnComputer(library, peer);
            taken = *ownComputer;
        }
        // A plugin's data over its limits is refused as one not signed is (#76).
        if (taken && PluginData::isPluginSpace(op.space))
            taken = PluginData::whyRefused(op).isEmpty();
        if (taken)
            admitted.ops << op;
        else
            ++admitted.refused;
    }
    return admitted;
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
            hashes << file.hash << file.original; // and the hash it came in with (#74)
        hashes.removeAll(QString());
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
        const Progress read = placeOf(*book);
        const auto row =
            std::tuple{query.value(2).toInt(), query.value(3).toDouble(), query.value(4).toDouble(), query.value(5).toLongLong()};
        if (query.value(2).isNull() || row != std::tuple{read.place.section, read.place.fraction, read.percent, read.updated})
            changes << PlaceChange{.book = query.value(0).toLongLong(), .place = read};
    }
    return changes;
}
