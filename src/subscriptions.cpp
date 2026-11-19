#include "subscriptions.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <optional>
#include <tuple>

#include "bookmatch.h"
#include "library.h"
#include "spacereducer.h"

using namespace Qt::StringLiterals;

namespace {

using Subscriptions::Dropped;
using Subscriptions::Options;
using Subscriptions::Placed;
using Subscriptions::Report;

template <typename Thing> [[nodiscard]] bool alive(const Thing &thing) { return !SpaceReducer::isRemoved(thing.alive, thing.removed); }

[[nodiscard]] QString text(const QHash<QString, SpaceStamp> &fields, const QString &field) { return fields.value(field).value; }

[[nodiscard]] QJsonObject object(const QString &json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }

// What the layer holds, as the subscriber's collection takes it: the space's
// collections by their path below its top one, its books, and of each book
// the collections below the top that hold it (none: the top itself).
struct Layer {
    QString root;
    QHash<QString, QString> paths; // a collection's id in the space, its path below the top ("" for the top)
    QHash<QString, QJsonObject> shelves;
    QList<BookMatch::Known> books;
    QList<QStringList> placedIn; // of each book, in the order of `books`
    // The books the layer held before this snapshot: filed here then, so what
    // became of them since is the subscriber's doing, and stays.
    QSet<QString> filed;
    qsizetype things = 0, facts = 0, relations = 0;
};

[[nodiscard]] QString topOf(const ReducedState &state, const QString &name) {
    QString top;
    for (auto collection = state.collections.cbegin(); collection != state.collections.cend(); ++collection) {
        if (!alive(*collection) || !text(collection->fields, u"parent"_s).isEmpty())
            continue;
        if (top.isEmpty() || text(collection->fields, u"name"_s) == name)
            top = collection.key();
    }
    return top;
}

// A collection's path below the top, or none for one outside the tree.
[[nodiscard]] std::optional<QString> pathBelow(const ReducedState &state, const QString &root, QString gid) {
    QStringList names;
    for (qsizetype depth = 0; depth <= state.collections.size(); ++depth) {
        if (gid == root)
            return names.join(u'/');
        const auto collection = state.collections.constFind(gid);
        if (collection == state.collections.cend() || !alive(*collection))
            return std::nullopt;
        names.prepend(text(collection->fields, u"name"_s).simplified().replace(u'/', u'-'));
        gid = text(collection->fields, u"parent"_s);
    }
    return std::nullopt;
}

[[nodiscard]] BookMatch::Known knownBook(const QString &gid, const ReducedBook &book) {
    BookMeta meta;
    meta.title = text(book.fields, u"title"_s);
    meta.authors = BookMeta::splitAuthors(text(book.fields, u"authors"_s));
    meta.series = text(book.fields, u"series"_s);
    meta.seriesIndex = text(book.fields, u"series_index"_s).toDouble();
    meta.isbn = text(book.fields, u"isbn"_s);
    return {.gid = gid, .meta = meta, .identifiers = object(text(book.fields, u"identifiers"_s))};
}

// Which of the space's collections below the top hold each book: by its rule
// (a search, read of what the space says of the book) or by hand.
void place(const ReducedState &state, Layer &layer) {
    QHash<QString, qint64> numbers;
    QList<Book> books;
    for (qsizetype at = 0; at < layer.books.size(); ++at) {
        numbers.insert(layer.books.at(at).gid, -(at + 1));
        books << Book{.id = -(at + 1), .meta = layer.books.at(at).meta, .identifiers = layer.books.at(at).identifiers};
    }
    layer.placedIn = QList<QStringList>(books.size());
    for (auto path = layer.paths.cbegin(); path != layer.paths.cend(); ++path) {
        if (path.key() == layer.root)
            continue;
        const ReducedCollection collection = state.collections.value(path.key());
        Collection::Part part{.rule = text(collection.fields, u"rule"_s), .included = {}, .excluded = {}};
        for (auto book = collection.books.cbegin(); book != collection.books.cend(); ++book)
            (book->value == "out"_L1 ? part.excluded : part.included).insert(numbers.value(book.key()));
        for (qsizetype at = 0; at < books.size(); ++at)
            if (part.holds(books.at(at)))
                layer.placedIn[at] << path.value();
    }
    for (QStringList &paths : layer.placedIn) {
        paths.sort();
        if (paths.isEmpty())
            paths << QString();
    }
}

[[nodiscard]] Layer layerOf(const ReducedState &state, const QString &name) {
    Layer layer;
    layer.root = topOf(state, name);
    for (auto collection = state.collections.cbegin(); collection != state.collections.cend(); ++collection)
        if (const auto path = pathBelow(state, layer.root, collection.key())) {
            layer.paths.insert(collection.key(), *path);
            layer.shelves.insert(collection.key(), object(text(collection->fields, u"shelf"_s)));
        }
    for (auto book = state.books.cbegin(); book != state.books.cend(); ++book)
        if (alive(*book) && !text(book->fields, u"title"_s).isEmpty())
            layer.books << knownBook(book.key(), *book);
    place(state, layer);
    layer.things = std::ranges::count_if(state.nodes, [](const ReducedNode &node) { return alive(node); });
    layer.facts = std::ranges::count_if(state.facts, [](const ReducedFact &fact) { return alive(fact); });
    layer.relations = std::ranges::count_if(state.edges, [](const ReducedEdge &edge) { return alive(edge); });
    return layer;
}

// The collection here that takes the space: the one named, else the one
// linked to the space, else one of the space's name; by its path, and its
// number when it is there already.
struct Target {
    QString path;
    qint64 id = 0;
};

[[nodiscard]] Target targetFor(const Library &library, const SubscribedSpace::Subscription &space, const Options &options) {
    if (options.as.trimmed().isEmpty())
        if (const auto linked = library.collection(SubscribedSpace::linkedCollection(library, space.space)))
            return {.path = linked->path, .id = linked->id};
    QString name = options.as.simplified();
    if (name.isEmpty())
        name = space.name.trimmed().isEmpty() ? u"Shared"_s : space.name.simplified().replace(u'/', u'-');
    if (const auto existing = library.collectionNamed(name))
        return {.path = existing->path, .id = existing->id};
    return {.path = name, .id = 0};
}

[[nodiscard]] QString below(const QString &top, const QString &path) { return path.isEmpty() ? top : top + u'/' + path; }

[[nodiscard]] bool removedHere(const Library &library, const BookMatch::Known &book) {
    const QStringList keys = Library::entryKeys(book.identifiers, book.meta.isbn, book.meta.title, book.meta.authors);
    return std::ranges::any_of(keys, [&library](const QString &key) { return library.wasRemoved(key); });
}

// What becomes of a book of the space that isn't the subscriber's: a
// wishlist entry, unless they removed it, or it was filed before and is gone.
[[nodiscard]] QString unmatched(const Library &library, const Layer &layer, const BookMatch::Known &book) {
    if (removedHere(library, book))
        return u"removed here before"_s;
    return layer.filed.contains(book.gid) ? u"not here any more"_s : u"wishlist"_s;
}

// A wishlist entry: a book without a file, known by what the space says of it.
[[nodiscard]] qint64 wish(Library &library, const BookMatch::Known &book) {
    const Library::Added added = library.addEntry(book.meta, false);
    for (auto identifier = book.identifiers.begin(); added.id > 0 && identifier != book.identifiers.end(); ++identifier)
        std::ignore = library.setIdentifier(added.id, identifier.key(),
                                            identifier.value().toVariant().toString()); // a name elsewhere is a help, not a need
    return added.id;
}

// What the subscriber's collection is, made where it isn't there yet.
[[nodiscard]] std::expected<qint64, QString> ensureCollection(Library &library, const QString &path, const QJsonObject &shelf) {
    if (const auto existing = library.collectionNamed(path); existing && existing->path.compare(path, Qt::CaseInsensitive) == 0)
        return existing->id;
    const auto made = library.addCollection(path);
    if (made && !shelf.isEmpty())
        std::ignore = library.setCollectionShelf(*made, shelf); // how the owner's shelf showed it, a nicety
    return made;
}

void putIn(Library &library, qint64 collectionId, qint64 bookId) {
    const auto collection = library.collection(collectionId);
    const auto book = library.book(bookId);
    // What the subscriber chose by hand stays: a book taken out isn't put back.
    if (collection && book && !collection->excluded.contains(bookId) && !collection->contains(*book))
        std::ignore = library.putInCollection(collectionId, bookId, true); // a book that can't be put in is left where it is
}

[[nodiscard]] Placed placed(const BookMatch::Known &book, const BookMatch::Found &found, const QStringList &paths, const QString &top) {
    Placed one{.gid = book.gid,
               .title = book.meta.title,
               .authors = book.meta.authorText(),
               .how = BookMatch::describe(found.how),
               .book = found.book,
               .collections = {}};
    for (const QString &path : paths)
        one.collections << below(top, path);
    return one;
}

// Writes the subscriber's collection and what the space's books are here.
[[nodiscard]] std::expected<void, QString> takeIn(Library &library, const Layer &layer, const QList<BookMatch::Found> &found,
                                                  Report &report) {
    const auto top = ensureCollection(library, report.collection, layer.shelves.value(layer.root));
    if (!top)
        return std::unexpected(top.error());
    SubscribedSpace::link(library, report.space.space, *top);
    report.space.collection = *top;
    if (const auto made = library.collection(*top))
        report.collection = made->path;
    QHash<QString, qint64> collections{{QString(), *top}};
    for (auto path = layer.paths.cbegin(); path != layer.paths.cend(); ++path)
        if (!path.value().isEmpty()) {
            const auto made = ensureCollection(library, below(report.collection, path.value()), layer.shelves.value(path.key()));
            if (!made)
                return std::unexpected(made.error());
            collections.insert(path.value(), *made);
        }
    for (qsizetype at = 0; at < layer.books.size(); ++at) {
        const BookMatch::Known &book = layer.books.at(at);
        Placed one = placed(book, found.at(at), layer.placedIn.at(at), report.collection);
        if (one.book == 0)
            one.how = unmatched(library, layer, book);
        if (one.book == 0 && one.how == "wishlist"_L1)
            one.book = wish(library, book);
        SubscribedSpace::placeBook(library, report.space.space, book.gid, one.book);
        for (const QString &path : layer.placedIn.at(at))
            if (one.book > 0 && !layer.filed.contains(book.gid))
                putIn(library, collections.value(path), one.book);
        report.books << one;
    }
    return {};
}

} // namespace

std::expected<SpaceSnapshot, QString> Subscriptions::fetch(const QString &source) {
    if (const QUrl url(source); !url.scheme().isEmpty() && url.scheme().size() > 1 && !url.isLocalFile())
        return std::unexpected(u"This link is not one Omabook can import from (a gist or a file of a GitHub repository): %1"_s.arg(source));
    const QString path = source.startsWith("file:"_L1) ? QUrl(source).toLocalFile() : source;
    if (!QFileInfo(path).isFile())
        return std::unexpected(u"There is no file %1 to import."_s.arg(path));
    return SpaceSnapshotFile::load(path);
}

std::expected<Report, QString> Subscriptions::subscribe(Library &library, const SpaceSnapshot &snapshot, const Options &options) {
    if (const auto verified = SubscribedSpace::verify(library, snapshot); !verified)
        return std::unexpected(verified.error());
    Filing filing{.space = {.space = snapshot.space,
                            .name = snapshot.name,
                            .owner = snapshot.owner.person,
                            .ownerName = snapshot.owner.name,
                            .collection = 0},
                  .before = SubscribedSpace::operations(library, snapshot.space),
                  .after = {}};
    filing.after = filing.before + snapshot.operations;
    if (options.dryRun) {
        auto report = file(library, filing, options);
        const qsizetype lacking = SubscribedSpace::lacking(library, snapshot).size();
        if (report)
            report->operations = {.added = lacking, .known = snapshot.operations.size() - lacking};
        return report;
    }
    const Library::Batch batch(library);
    const auto accepted = SubscribedSpace::accept(library, snapshot);
    if (!accepted)
        return std::unexpected(accepted.error());
    auto report = file(library, filing, options);
    if (report)
        report->operations = *accepted;
    return report;
}

std::expected<Report, QString> Subscriptions::file(Library &library, const Filing &filing, const Options &options) {
    const ReducedState before = SpaceReducer::reduce(filing.before);
    Layer layer = layerOf(SpaceReducer::reduce(filing.after), filing.space.name);
    for (auto book = before.books.cbegin(); book != before.books.cend(); ++book)
        layer.filed.insert(book.key());
    const QList<BookMatch::Found> found = BookMatch::find(library, layer.books);
    const Target target = targetFor(library, filing.space, options);
    Report report{.space = filing.space,
                  .collection = target.path,
                  .newCollection = target.id == 0,
                  .dryRun = options.dryRun,
                  .operations = {},
                  .books = {},
                  .things = layer.things,
                  .facts = layer.facts,
                  .relations = layer.relations};
    report.space.collection = target.id;
    if (options.dryRun) {
        for (qsizetype at = 0; at < layer.books.size(); ++at) {
            Placed one = placed(layer.books.at(at), found.at(at), layer.placedIn.at(at), target.path);
            if (one.book == 0)
                one.how = unmatched(library, layer, layer.books.at(at));
            report.books << one;
        }
        return report;
    }
    const Library::Batch batch(library);
    if (const auto taken = takeIn(library, layer, found, report); !taken)
        return std::unexpected(taken.error());
    return report;
}

std::expected<SubscribedSpace::Subscription, QString> Subscriptions::named(const Library &library, const QString &which) {
    const QList<SubscribedSpace::Subscription> subscriptions = SubscribedSpace::list(library);
    const auto collection = library.collectionNamed(which);
    QList<SubscribedSpace::Subscription> meant;
    for (const SubscribedSpace::Subscription &subscription : subscriptions)
        if (subscription.space == which || subscription.name.compare(which.simplified(), Qt::CaseInsensitive) == 0
            || (collection && subscription.collection == collection->id))
            meant << subscription;
    if (meant.isEmpty())
        return std::unexpected(u"This library subscribes to no space called %1."_s.arg(which));
    if (meant.size() > 1) {
        QStringList ids;
        for (const SubscribedSpace::Subscription &subscription : std::as_const(meant))
            ids << subscription.space;
        return std::unexpected(u"More than one space goes by %1; name one by its id: %2"_s.arg(which, ids.join(", "_L1)));
    }
    return meant.first();
}

std::expected<Dropped, QString> Subscriptions::unsubscribe(const Library &library, const QString &which) {
    const auto meant = named(library, which);
    if (!meant)
        return std::unexpected(meant.error());
    const SubscribedSpace::Subscription &space = *meant;
    const auto kept = space.collection > 0 ? library.collection(space.collection) : std::nullopt;
    const auto dropped = SubscribedSpace::drop(library, space.space);
    if (!dropped)
        return std::unexpected(dropped.error());
    return Dropped{.space = space, .collection = kept ? kept->path : QString(), .operations = *dropped};
}

QJsonObject Subscriptions::toJson(const Report &report) {
    QJsonArray books;
    qsizetype matched = 0;
    for (const Placed &book : report.books) {
        books << QJsonObject{{"title", book.title},
                             {"authors", book.authors},
                             {"how", book.how},
                             {"id", book.book},
                             {"collections", QJsonArray::fromStringList(book.collections)}};
        matched += book.how != "wishlist"_L1 && book.how != "removed here before"_L1 ? 1 : 0;
    }
    return {
        {"space", report.space.space},
        {"name", report.space.name},
        {"owner", QJsonObject{{"name", report.space.ownerName}, {"person", report.space.owner}}},
        {"collection", report.collection},
        {"new_collection", report.newCollection},
        {"dry_run", report.dryRun},
        {"operations", QJsonObject{{"added", report.operations.added}, {"known", report.operations.known}}},
        {"books", books},
        {"matched", matched},
        {"wishlist", std::ranges::count_if(report.books, [](const Placed &book) { return book.how == "wishlist"_L1; })},
        {"graph", QJsonObject{{"things", report.things},
                              {"facts", report.facts},
                              {"relations", report.relations},
                              {"shown", true},
                              {"kept", "in the subscribed space, apart from your own graph, shown as far as you have read each book"}}}};
}

QJsonObject Subscriptions::toJson(const Dropped &dropped) {
    return {{"space", dropped.space.space},
            {"name", dropped.space.name},
            {"collection", dropped.collection},
            {"operations", dropped.operations}};
}

QStringList Subscriptions::describe(const Report &report) {
    const QString owner = report.space.ownerName.isEmpty() ? u"its owner"_s : report.space.ownerName;
    QStringList lines{
        u"%1 %2, published by %3 (%4): %5 operations new, %6 already here, kept apart from your own library."_s
            .arg(report.dryRun ? u"Would subscribe to"_s : u"Subscribed to"_s, report.space.name, owner, report.space.space)
            .arg(report.operations.added)
            .arg(report.operations.known),
        u"Its books go in the collection %1%2:"_s.arg(report.collection, report.newCollection ? u" (new)"_s : u" (already here)"_s)};
    for (const Placed &book : report.books)
        lines << u"  %1\t%2\t%3\t%4\t%5"_s.arg(book.book > 0 ? QString::number(book.book) : u"-"_s, book.title, book.authors, book.how,
                                               book.collections.join(", "_L1));
    if (report.things + report.facts + report.relations > 0)
        lines << u"Its knowledge graph (%1 things, %2 facts, %3 relations) stays in the space, apart from yours, and is shown "
                 "beside it as far as you have read each book."_s.arg(report.things)
                     .arg(report.facts)
                     .arg(report.relations);
    if (report.dryRun)
        lines << u"Nothing was written (--dry-run)."_s;
    return lines;
}

QString Subscriptions::describe(const Dropped &dropped) {
    return u"Unsubscribed from %1: its %2 operations are gone from this library.%3"_s.arg(dropped.space.name)
        .arg(dropped.operations)
        .arg(dropped.collection.isEmpty()
                 ? QString()
                 : u" The collection %1 stays, with its books and wishlist entries: it is yours."_s.arg(dropped.collection));
}
