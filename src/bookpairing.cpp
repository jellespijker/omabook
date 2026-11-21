#include "bookpairing.h"

#include <QSet>
#include <algorithm>
#include <optional>

#include "bookidentity.h"
#include "booklinks.h"
#include "changelog.h"
#include "hlc.h"
#include "library.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// The books here as a pairing looks at them, and what it has linked so far.
struct Here {
    QHash<QString, QString> canonical; // the log's same-as links it follows, and the ones made since
    QSet<QString> apart;               // the pairs of ids the log says are not one book
    QHash<qint64, QString> gids;
    QHash<qint64, QString> titles;
    QHash<QString, qint64> byId;         // each book here by the id it stands for
    QHash<QString, QList<qint64>> byKey; // each strong key, the books here that have it
    QSet<QString> held;                  // the ids their books stand for

    [[nodiscard]] QString standsFor(const QString &gid) const { return canonical.value(gid, gid); }

    // Of a book's linked group, every id now stands for the smaller of the two
    // roots, as SpaceReducer::sameAs would answer with the new link among the rest.
    void join(const QString &one, const QString &other) {
        const QString first = standsFor(one);
        const QString second = standsFor(other);
        const QString root = std::min(first, second);
        const QString joined = std::max(first, second);
        for (QString &stands : canonical)
            if (stands == joined)
                stands = root;
        for (const QString &id : {one, other, first, second})
            canonical.insert(id, root);
    }
};

[[nodiscard]] Here here(const Library &library, std::span<const SpaceOperation> ops, const QSet<QString> &theirGids) {
    Here found{.canonical = SpaceReducer::sameAs(BookLinks::trusted(ops)),
               .apart = BookLinks::apart(ops),
               .gids = {},
               .titles = {},
               .byId = {},
               .byKey = {},
               .held = {}};
    for (const Book &book : library.books()) {
        const QString gid = Spaces::bookId(library, book.id);
        if (gid.isEmpty())
            continue;
        found.gids.insert(book.id, gid);
        found.titles.insert(book.id, book.meta.title);
        found.byId.insert(found.standsFor(gid), book.id);
        for (const QString &key : BookIdentity::strongKeys(book))
            found.byKey[key] << book.id;
    }
    for (const QString &gid : theirGids)
        found.held.insert(found.standsFor(gid));
    return found;
}

// The book here with a file of the same content as one of theirs.
[[nodiscard]] qint64 byContent(const Library &library, const QStringList &hashes) {
    for (const QString &hash : hashes)
        if (const qint64 mine = library.bookWithHash(hash); mine > 0)
            return mine;
    return 0;
}

// The book here that their book without a file is, met for the first time:
// the one book that each of their keys that names one book here names.
[[nodiscard]] std::optional<qint64> byKeys(const Here &found, const BookPairing::Names &their, BookPairing::Pairs &pairs) {
    if (found.byId.contains(found.standsFor(their.gid)))
        return std::nullopt;
    QSet<qint64> named;
    QString by;
    for (const QString &key : their.keys)
        if (const QList<qint64> books = found.byKey.value(key); books.size() == 1) {
            named.insert(books.first());
            by = key;
        }
    if (named.size() != 1)
        return std::nullopt;
    const qint64 mine = *named.cbegin();
    const QString gid = found.gids.value(mine);
    // Paired already: with another of theirs by a link, or by its own id there.
    if (found.canonical.contains(gid) || found.held.contains(found.standsFor(gid))
        || found.apart.contains(BookLinks::pairOf(gid, their.gid)))
        return std::nullopt;
    if (!BookIdentity::titlesAgree(found.titles.value(mine), their.title)) {
        // Named alike from either side.
        const QString one = found.titles.value(mine);
        pairs.apart << std::min(one, their.title) + u" / "_s + std::max(one, their.title) + u" ("_s + by.section(u':', 1) + u')';
        return std::nullopt;
    }
    return mine;
}

} // namespace

QHash<qint64, QStringList> BookPairing::keysAlone(const Library &library) {
    const QList<Book> books = library.books();
    QHash<QString, int> count;
    for (const Book &book : books)
        for (const QString &key : BookIdentity::strongKeys(book))
            ++count[key];
    QHash<qint64, QStringList> alone;
    for (const Book &book : books) {
        QStringList keys = BookIdentity::strongKeys(book);
        keys.removeIf([&count](const QString &key) { return count.value(key) != 1; });
        alone.insert(book.id, keys);
    }
    return alone;
}

BookPairing::Pairs BookPairing::pairs(const Library &library, std::span<const SpaceOperation> ops, const QList<Names> &theirs,
                                      const QSet<QString> &theirGids) {
    Here found = here(library, ops, theirGids);
    Pairs pairs;
    for (const Names &their : theirs) {
        const std::optional<qint64> mine =
            their.hashes.isEmpty() ? byKeys(found, their, pairs) : std::optional(byContent(library, their.hashes));
        const QString gid = mine ? found.gids.value(*mine) : QString();
        if (gid.isEmpty() || their.gid.isEmpty() || found.standsFor(gid) == found.standsFor(their.gid))
            continue;
        pairs.links << std::pair{gid, their.gid};
        found.join(gid, their.gid);
    }
    return pairs;
}

QList<SpaceOperation> BookPairing::asOperations(const Library &library, const Pairs &pairs, qint64 at) {
    QList<SpaceOperation> ops;
    const qint64 now = std::max(Hlc::pack(Spaces::wallSeconds(library)), at);
    for (const auto &[mine, theirs] : pairs.links)
        ops << SpaceOperation{.space = Spaces::personalSpace(library),
                              .device = Spaces::deviceId(library),
                              .seq = 0,
                              .hlc = now,
                              .author = {},
                              .entity = u"book"_s,
                              .id = std::max(mine, theirs),
                              .field = u"same_as"_s,
                              .value = std::min(mine, theirs)};
    return ops;
}

int BookPairing::record(const Library &library, const Pairs &pairs) {
    const Library::Batch batch(library);
    for (const auto &[mine, theirs] : pairs.links)
        ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                    .entity = u"book"_s,
                                    .id = std::max(mine, theirs),
                                    .field = u"same_as"_s,
                                    .value = std::min(mine, theirs),
                                    .hlc = std::nullopt});
    return static_cast<int>(pairs.links.size());
}
