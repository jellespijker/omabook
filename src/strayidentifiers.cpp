#include "strayidentifiers.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <utility>

#include "bookidentity.h"
#include "booklinks.h"
#include "library.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// Of each id the links fold books into, the strong keys it had: when it
// first had each by an identifier, and the ones that are its own, by its ISBN
// now or its id made of them (BookIdentity::idKeys). The log's first time alone
// would not do: compaction leaves only the last of an id's identifiers.
struct Had {
    QHash<QString, qint64> first;
    QSet<QString> own;
};

[[nodiscard]] QHash<QString, Had> hadBy(std::span<const SpaceOperation> ops, const QHash<QString, QString> &canonical) {
    QHash<QString, Had> had;
    QHash<QString, std::pair<qint64, QString>> isbns; // each id's ISBN now
    for (const SpaceOperation &op : ops) {
        if (op.entity != "book"_L1)
            continue;
        Had &book = had[canonical.value(op.id, op.id)];
        if (op.id.startsWith("e:entry:"_L1))
            book.own.insert(op.id.mid(2));
        if (op.field == "isbn"_L1 && op.hlc >= isbns.value(op.id).first)
            isbns.insert(op.id, {op.hlc, op.value});
        if (op.field != "identifiers"_L1)
            continue;
        for (const QString &key : BookIdentity::strongKeys(QJsonDocument::fromJson(op.value.toUtf8()).object(), {}))
            if (const auto at = book.first.constFind(key); at == book.first.cend() || op.hlc < *at)
                book.first.insert(key, op.hlc);
    }
    for (auto isbn = isbns.cbegin(); isbn != isbns.cend(); ++isbn)
        for (const QString &key : BookIdentity::strongKeys(QJsonObject(), isbn->second))
            had[canonical.value(isbn.key(), isbn.key())].own.insert(key);
    return had;
}

// Of two books kept apart, the identifiers `one` has of `other`: of the
// keys both had, one that only the other has as its own, or that neither
// has so and the other had first.
void strayOf(const Had &one, const Had &other, QSet<QString> &stray) {
    for (auto key = one.first.cbegin(); key != one.first.cend(); ++key) {
        if (one.own.contains(key.key()))
            continue;
        const auto there = other.first.constFind(key.key());
        if (other.own.contains(key.key()) || (there != other.first.cend() && *key > *there))
            stray.insert(key.key());
    }
}

[[nodiscard]] QHash<QString, QSet<QString>> strayBy(std::span<const SpaceOperation> ops, const QHash<QString, QString> &canonical) {
    const QHash<QString, Had> had = hadBy(ops, canonical);
    QHash<QString, QSet<QString>> stray;
    for (const SpaceOperation &op : ops) {
        if (op.entity != "book"_L1 || op.field != "not_same_as"_L1 || op.value.isEmpty())
            continue;
        const QString one = canonical.value(op.id, op.id);
        const QString other = canonical.value(op.value, op.value);
        if (one == other)
            continue;
        strayOf(had.value(one), had.value(other), stray[one]);
        strayOf(had.value(other), had.value(one), stray[other]);
    }
    stray.removeIf([](const auto &keys) { return keys.value().isEmpty(); });
    return stray;
}

} // namespace

QHash<QString, QSet<QString>> StrayIdentifiers::of(std::span<const SpaceOperation> ops) {
    return strayBy(ops, SpaceReducer::sameAs(BookLinks::trusted(ops)));
}

bool StrayIdentifiers::isStray(const QSet<QString> &stray, const QString &scheme, const QString &value) {
    return std::ranges::any_of(BookIdentity::strongKeys(QJsonObject{{scheme, value}}, {}),
                               [&stray](const QString &key) { return stray.contains(key); });
}

QStringList StrayIdentifiers::remove(Library &library, std::span<const SpaceOperation> ops, bool dryRun) {
    const QHash<QString, QString> canonical = SpaceReducer::sameAs(BookLinks::trusted(ops));
    const QHash<QString, QSet<QString>> stray = strayBy(ops, canonical);
    if (stray.isEmpty())
        return {};
    QStringList removed;
    const Library::Batch batch(library);
    for (const Book &book : library.books()) {
        const QString gid = Spaces::bookId(library, book.id);
        const auto found = stray.constFind(canonical.value(gid, gid));
        if (found == stray.cend())
            continue;
        for (auto it = book.identifiers.begin(); it != book.identifiers.end(); ++it) {
            const QString value = it.value().toVariant().toString();
            if (!isStray(*found, it.key(), value))
                continue;
            removed << book.meta.title + u": "_s + it.key() + u' ' + value;
            if (!dryRun)
                library.setIdentifier(book.id, it.key(), {});
        }
    }
    return removed;
}
