#include "bookspaces.h"

#include <QSet>
#include <optional>

#include "library.h"
#include "privatebooks.h"
#include "publishedlog.h"
#include "sharedspace.h"
#include "spaces.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

// The collection here that stands for a space: the one linked to it (one
// subscribed to, or a shared space's), else the one this library publishes as it.
[[nodiscard]] qint64 collectionOf(const Library &library, const QString &space) {
    if (const qint64 linked = SubscribedSpace::linkedCollection(library, space); linked > 0)
        return linked;
    const std::optional<PublishedLog::Record> published = PublishedLog::recorded(library, space);
    return published ? Spaces::localId(library, u"collection"_s, published->collection) : 0;
}

} // namespace

QList<BookSpaces::Held> BookSpaces::of(const Library &library, qint64 book) {
    const std::optional<Book> found = library.book(book);
    if (!found || PrivateBooks::withheld(library, {*found}).contains(book))
        return {};
    QSet<qint64> holding;
    for (const Collection &collection : library.collectionsOf(*found))
        holding.insert(collection.id);
    QList<Held> held;
    for (const Spaces::Recorded &space : Spaces::recorded(library)) {
        if (space.kind != "published"_L1 && space.kind != QLatin1StringView(SharedSpace::kind))
            continue;
        const qint64 collection = collectionOf(library, space.id);
        if (collection > 0 && holding.contains(collection))
            held.append({.space = space.id, .kind = space.kind, .name = space.name, .collection = collection});
    }
    return held;
}
