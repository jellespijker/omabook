#include "sharedcollection.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>
#include <optional>

#include "changelog.h"
#include "library.h"
#include "publishedspace.h"
#include "sharedspace.h"
#include "spacereducer.h"
#include "spaces.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString keyOf(const SpaceOperation &op) { return op.entity + u'\x1f' + op.id + u'\x1f' + op.field; }

// Two values the same, also when one is JSON written another way.
[[nodiscard]] bool same(const QString &one, const QString &other) {
    if (one == other)
        return true;
    const QJsonDocument first = QJsonDocument::fromJson(one.toUtf8());
    const QJsonDocument second = QJsonDocument::fromJson(other.toUtf8());
    return !first.isNull() && first == second;
}

// What the space says now, of each field the latest and of what grows each value.
struct Said {
    QHash<QString, SpaceStamp> latest;
    QSet<QString> grown;

    explicit Said(const QList<SpaceOperation> &ops) {
        for (const SpaceOperation &op : ops) {
            if (SpaceReducer::grows(op.field)) {
                grown.insert(keyOf(op) + u'\x1f' + op.value);
                continue;
            }
            const SpaceStamp stamp{.hlc = op.hlc, .device = op.device, .priority = 0, .value = op.value};
            if (const auto at = latest.constFind(keyOf(op)); at == latest.cend() || *at < stamp)
                latest.insert(keyOf(op), stamp);
        }
    }

    // Whether the writer's `op`, as it is at home, is news to the space.
    [[nodiscard]] bool isNews(const SpaceOperation &op) const {
        if (SpaceReducer::grows(op.field))
            return !grown.contains(keyOf(op) + u'\x1f' + op.value);
        const auto at = latest.constFind(keyOf(op));
        if (at == latest.cend())
            return !op.value.isEmpty();
        return !same(at->value, op.value) && op.hlc > at->hlc;
    }
};

// The space's collections by their path below its top one ("" for the top).
[[nodiscard]] QHash<QString, QString> spacePaths(const ReducedState &state, QString &top) {
    QHash<QString, QString> paths;
    const auto name = [&state](const QString &gid) { return state.collections.value(gid).fields.value(u"name"_s).value; };
    const auto parent = [&state](const QString &gid) { return state.collections.value(gid).fields.value(u"parent"_s).value; };
    for (auto collection = state.collections.cbegin(); collection != state.collections.cend(); ++collection)
        if (!SpaceReducer::isRemoved(collection->alive, collection->removed) && parent(collection.key()).isEmpty()
            && (top.isEmpty() || collection.key() < top))
            top = collection.key();
    for (auto collection = state.collections.cbegin(); collection != state.collections.cend(); ++collection) {
        QStringList names;
        QString at = collection.key();
        for (qsizetype depth = 0; depth <= state.collections.size() && !at.isEmpty() && at != top; ++depth) {
            names.prepend(name(at));
            at = parent(at);
        }
        if (at == top && !top.isEmpty())
            paths.insert(names.join(u'/'), collection.key());
    }
    return paths;
}

// Of each collection here in the linked tree, the id it has in the space as
// publishing names it, and the id the space knows it by: linked before, else
// the space's one at the same path, else a new one, linked from now on.
[[nodiscard]] QHash<QString, QString> collectionIds(const Library &library, const QString &space, qint64 root, const ReducedState &state,
                                                    QString &top) {
    const QHash<QString, QString> byPath = spacePaths(state, top);
    if (top.isEmpty())
        top = PublishedSpace::collectionIdIn(space, u"/"_s);
    const QString linkEntity = u"in:"_s + space;
    QHash<QString, QString> ids;
    const std::optional<Collection> rootHere = library.collection(root);
    if (!rootHere)
        return {};
    for (const Collection &collection : library.collections()) {
        const bool inTree = collection.id == root || collection.path.startsWith(rootHere->path + u'/');
        if (!inTree)
            continue;
        const QString below = collection.id == root ? QString() : collection.path.mid(rootHere->path.size() + 1);
        QString id = collection.id == root ? top : Spaces::globalId(library, linkEntity, collection.id);
        if (id.isEmpty())
            id = byPath.value(below, PublishedSpace::collectionIdIn(space, u"/"_s + below));
        if (collection.id != root)
            Spaces::setGlobalId(library, linkEntity, collection.id, id);
        ids.insert(PublishedSpace::collectionIdIn(space, Spaces::collectionId(library, collection.id)), id);
    }
    return ids;
}

// The operation as the space names things, or none for one that stays home.
[[nodiscard]] std::optional<SpaceOperation> inSpace(SpaceOperation op, const QHash<QString, QString> &books,
                                                    const QHash<QString, QString> &collections) {
    if (op.field == "same_as"_L1)
        return std::nullopt; // how this library lines up its own ids
    if (op.entity == "book"_L1)
        op.id = books.value(op.id, op.id);
    if (op.entity != "collection"_L1)
        return op;
    if (!collections.contains(op.id))
        return std::nullopt;
    op.id = collections.value(op.id);
    if (op.field == "parent"_L1 && !op.value.isEmpty())
        op.value = collections.value(op.value, op.value);
    if (op.field.startsWith("book:"_L1))
        op.field = u"book:"_s + books.value(op.field.mid(5), op.field.mid(5));
    return op;
}

// Whether a writer's operation, as the space names things, goes into it.
// The space's top is called as it was founded and sits at the top; each
// member's own may go by another name, or sit elsewhere, here. And a member
// who lacks a book's file (a wishlist entry of the space's book) says
// nothing of the files others have (#63).
[[nodiscard]] bool goes(const std::optional<SpaceOperation> &op, const QString &top) {
    if (!op || (op->id == top && (op->field == "name"_L1 || op->field == "parent"_L1)))
        return false;
    return op->entity != "book"_L1 || op->field != "formats"_L1 || !QJsonDocument::fromJson(op->value.toUtf8()).array().isEmpty();
}

} // namespace

std::expected<Subscriptions::Report, QString> SharedCollection::file(Library &library, const QString &space,
                                                                     const QList<SpaceOperation> &before, const QString &as) {
    const SharedSpace::State now = SharedSpace::state(library, space);
    // Before its founding is here, the space goes by the name its invite gave it.
    QString name = now.name;
    for (const Spaces::Recorded &recorded : Spaces::recorded(library))
        if (name.isEmpty() && recorded.id == space)
            name = recorded.name;
    const Subscriptions::Filing filing{.space = {.space = space,
                                                 .name = name.isEmpty() ? space : name,
                                                 .owner = now.founder,
                                                 .ownerName = {},
                                                 .collection = SubscribedSpace::linkedCollection(library, space)},
                                       .before = before,
                                       .after = now.accepted};
    return Subscriptions::file(library, filing, {.as = as, .dryRun = false});
}

std::expected<qsizetype, QString> SharedCollection::contribute(const Library &library, const QString &space,
                                                               const Subscriptions::Report &filed) {
    const qint64 root = SubscribedSpace::linkedCollection(library, space);
    if (root == 0)
        return std::unexpected(u"No collection here is linked to %1."_s.arg(space));
    const SharedSpace::State now = SharedSpace::state(library, space);
    PublishedSpace::Parts parts = PublishedSpace::Parts::fromNames(now.parts);
    parts.files = now.files;
    const auto home = PublishedSpace::operations(library, root, parts, space);
    if (!home)
        return std::unexpected(home.error());
    QHash<QString, QString> books;
    for (const Subscriptions::Placed &book : filed.books)
        if (book.book > 0 && !book.gid.isEmpty())
            books.insert(Spaces::bookId(library, book.book), book.gid);
    const ReducedState state = SpaceReducer::reduce(now.accepted);
    QString top;
    const QHash<QString, QString> collections = collectionIds(library, space, root, state, top);
    const bool hasTop = state.collections.contains(top);
    const Said said(now.accepted);
    QList<ChangeLog::Entry> entries;
    for (const SpaceOperation &op : *home) {
        const std::optional<SpaceOperation> theirs = inSpace(op, books, collections);
        if (!goes(theirs, top))
            continue;
        if (said.isNews(*theirs))
            entries << ChangeLog::Entry{
                .space = space, .entity = theirs->entity, .id = theirs->id, .field = theirs->field, .value = theirs->value, .hlc = {}};
    }
    if (!hasTop)
        entries.prepend({.space = space, .entity = u"collection"_s, .id = top, .field = u"name"_s, .value = now.name, .hlc = {}});
    if (entries.isEmpty())
        return 0;
    if (const auto written = SharedSpace::write(library, space, entries); !written)
        return std::unexpected(written.error());
    return entries.size();
}
