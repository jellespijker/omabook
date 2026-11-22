#include "synctakecollections.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <optional>
#include <tuple>

#include "readingformat.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString fieldOf(const ReducedCollection &collection, const QString &field) { return collection.fields.value(field).value; }

[[nodiscard]] QJsonObject objectOf(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

// A collection as the log says it is now, written over `row` when there is
// one, its books by the id they stand for here.
[[nodiscard]] Collection collectionFrom(const ReducedCollection &reduced, const Collection *row, const QHash<QString, qint64> &parents,
                                        const QHash<QString, qint64> &books, const QHash<QString, QString> &same) {
    Collection collection;
    collection.id = row ? row->id : 0;
    collection.name = fieldOf(reduced, u"name"_s);
    collection.parent = parents.value(fieldOf(reduced, u"parent"_s));
    collection.rule = fieldOf(reduced, u"rule"_s).trimmed();
    collection.shelf = objectOf(fieldOf(reduced, u"shelf"_s));
    collection.format = ReadingFormat::fitting(objectOf(fieldOf(reduced, u"format"_s)));
    collection.removed = SpaceReducer::isRemoved(reduced.alive, reduced.removed);
    collection.updated = std::max(reduced.alive.hlc, reduced.removed.hlc);
    // A removed one keeps only its name and place, as removing it leaves it.
    if (collection.removed) {
        collection.rule.clear();
        collection.shelf = {};
        collection.format = {};
        return collection;
    }
    for (auto book = reduced.books.cbegin(); book != reduced.books.cend(); ++book)
        if (const qint64 id = books.value(same.value(book.key(), book.key())); id > 0 && book->value == "in"_L1)
            collection.included.insert(id);
        else if (id > 0 && book->value == "out"_L1)
            collection.excluded.insert(id);
    return collection;
}

[[nodiscard]] bool sameCollection(const Collection &row, const Collection &log) {
    if (row.removed && log.removed)
        return true;
    return std::tie(row.name, row.parent, row.rule, row.removed, row.included, row.excluded)
               == std::tie(log.name, log.parent, log.rule, log.removed, log.included, log.excluded)
           && row.shelf == log.shelf && row.format == log.format;
}

} // namespace

int SyncTakeCollections::take(Library &library, const QMap<QString, ReducedCollection> &merged, const QHash<QString, qint64> &books,
                              const QHash<QString, QString> &same, bool dryRun) {
    int taken = 0;
    QHash<QString, Collection> rows;
    QHash<QString, qint64> ids;
    for (const Collection &collection : library.collections(true)) {
        const QString gid = Spaces::collectionId(library, collection.id);
        rows.insert(gid, collection);
        ids.insert(gid, collection.id);
    }
    QStringList order = merged.keys();
    std::ranges::stable_sort(order, {}, [](const QString &gid) { return gid.count(u'/'); });
    const Library::Batch batch(library);
    for (const QString &gid : std::as_const(order)) {
        const ReducedCollection &reduced = merged[gid];
        const auto row = rows.constFind(gid);
        const Collection log = collectionFrom(reduced, row == rows.cend() ? nullptr : &*row, ids, books, same);
        if ((row == rows.cend() && log.removed) || log.name.isEmpty() || (row != rows.cend() && sameCollection(*row, log)))
            continue;
        ++taken;
        if (dryRun)
            continue;
        if (const std::optional<qint64> stored = library.storeCollection(log)) {
            Spaces::setGlobalId(library, u"collection"_s, *stored, gid);
            ids.insert(gid, *stored);
        }
    }
    return taken;
}
