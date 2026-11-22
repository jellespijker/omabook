#include "strayidentifiers.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <optional>
#include <utility>

#include "bookidentity.h"
#include "booklinks.h"
#include "library.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// The strong keys an operation of a book gives it: of its identifiers, or of its ISBN.
[[nodiscard]] QStringList keysOf(const SpaceOperation &op) {
    if (op.field == "identifiers"_L1)
        return BookIdentity::strongKeys(QJsonDocument::fromJson(op.value.toUtf8()).object(), {});
    if (op.field == "isbn"_L1)
        return BookIdentity::strongKeys(QJsonObject(), op.value);
    return {};
}

// Of each id the links fold books into, the strong keys it had, when it first
// had each by an identifier, its titles now, and the keys that are its own by
// its id or its ISBN now. The log's first time alone would not do:
// compaction leaves only the last of an id's identifiers.
struct Had {
    QHash<QString, qint64> first;
    QSet<QString> own;
    QStringList titles;
};

// When each key was first recorded, and on which ids then.
using Firsts = QHash<QString, std::pair<qint64, QStringList>>;

void noteFirst(Firsts &firsts, const QString &key, const SpaceOperation &op) {
    const auto at = firsts.find(key);
    if (at == firsts.end() || op.hlc < at->first)
        firsts.insert(key, {op.hlc, {op.id}});
    else if (op.hlc == at->first && !at->second.contains(op.id))
        at->second << op.id;
}

struct Log {
    QHash<QString, Had> had;
    Firsts firsts;
};

[[nodiscard]] Log logOf(std::span<const SpaceOperation> ops, const QHash<QString, QString> &canonical, const BookLinks::Titles &titles) {
    Log log;
    QHash<QString, std::pair<qint64, QString>> isbns; // each id's ISBN now
    for (const SpaceOperation &op : ops) {
        if (op.entity != "book"_L1)
            continue;
        Had &book = log.had[canonical.value(op.id, op.id)];
        if (op.id.startsWith("e:entry:"_L1))
            book.own.insert(op.id.mid(2));
        if (op.field == "isbn"_L1 && op.hlc >= isbns.value(op.id).first)
            isbns.insert(op.id, {op.hlc, op.value});
        for (const QString &key : keysOf(op)) {
            noteFirst(log.firsts, key, op);
            if (const auto at = book.first.constFind(key); op.field == "identifiers"_L1 && (at == book.first.cend() || op.hlc < *at))
                book.first.insert(key, op.hlc);
        }
    }
    for (auto isbn = isbns.cbegin(); isbn != isbns.cend(); ++isbn)
        for (const QString &key : BookIdentity::strongKeys(QJsonObject(), isbn->second))
            log.had[canonical.value(isbn.key(), isbn.key())].own.insert(key);
    for (auto title = titles.cbegin(); title != titles.cend(); ++title)
        log.had[canonical.value(title.key(), title.key())].titles
            << std::ranges::max(*title, {}, &std::pair<qint64, QString>::first).second;
    return log;
}

[[nodiscard]] bool agreesWithAny(const QStringList &titles, const QString &title) {
    return std::ranges::any_of(titles, [&title](const QString &other) { return BookIdentity::titlesAgree(title, other); });
}

// Whether the key is the book's own as `had` says: by its id, its ISBN now,
// or a title it has now that agrees with one the key was first recorded under.
[[nodiscard]] bool ownedBy(const Had &had, const QString &key, const QStringList &origin) {
    return had.own.contains(key)
           || (had.first.contains(key) && std::ranges::any_of(had.titles, [&](const QString &t) { return agreesWithAny(origin, t); }));
}

} // namespace

StrayIdentifiers::StrayIdentifiers(std::span<const SpaceOperation> ops) {
    m_canonical = SpaceReducer::sameAs(BookLinks::trusted(ops));
    const QHash<QString, QString> &canonical = m_canonical;
    const BookLinks::Titles titles = BookLinks::titlesOf(ops);
    const Log log = logOf(ops, canonical, titles);
    for (auto first = log.firsts.cbegin(); first != log.firsts.cend(); ++first)
        for (const QString &id : first->second)
            if (const std::optional<QString> title = BookLinks::titleAt(titles, id, first->first))
                m_origin[first.key()] << *title;
    // Of two books kept apart, the keys `one` had that `other` has as its own
    // (Theirs), or neither and the other had first (Doubtful).
    const auto share = [&](const QString &here, const QString &apart) {
        const Had mine = log.had.value(here);
        const Had theirs = log.had.value(apart);
        for (auto key = mine.first.cbegin(); key != mine.first.cend(); ++key) {
            const auto there = theirs.first.constFind(key.key());
            if (ownedBy(theirs, key.key(), m_origin.value(key.key())))
                m_shared[here].insert(key.key(), Whose::Theirs);
            else if (there != theirs.first.cend() && *key > *there)
                m_shared[here].insert(key.key(), Whose::Doubtful);
        }
    };
    for (const SpaceOperation &op : ops) {
        if (op.entity != "book"_L1 || op.field != "not_same_as"_L1 || op.value.isEmpty())
            continue;
        const QString one = canonical.value(op.id, op.id);
        const QString other = canonical.value(op.value, op.value);
        if (one == other)
            continue;
        for (const auto &[here, apart] : {std::pair{one, other}, std::pair{other, one}})
            share(here, apart);
    }
}

bool StrayIdentifiers::owns(const Book &book, const QString &gid, const QString &key) const {
    return gid == u"e:"_s + key || BookIdentity::strongKeys(QJsonObject(), book.meta.isbn).contains(key)
           || agreesWithAny(m_origin.value(key), book.meta.title);
}

QSet<QString> StrayIdentifiers::keys(const Book &book, const QString &gid, const QString &standsFor, Whose whose) const {
    QSet<QString> keys;
    const QHash<QString, Whose> shared = m_shared.value(standsFor);
    for (auto key = shared.cbegin(); key != shared.cend(); ++key)
        if (*key == whose && !owns(book, gid, key.key()))
            keys.insert(key.key());
    return keys;
}

QSet<QString> StrayIdentifiers::of(const Book &book, const QString &gid, const QString &standsFor) const {
    return keys(book, gid, standsFor, Whose::Theirs);
}

QSet<QString> StrayIdentifiers::doubtful(const Book &book, const QString &gid, const QString &standsFor) const {
    return keys(book, gid, standsFor, Whose::Doubtful);
}

bool StrayIdentifiers::isStray(const QSet<QString> &stray, const QString &scheme, const QString &value) {
    return std::ranges::any_of(BookIdentity::strongKeys(QJsonObject{{scheme, value}}, {}),
                               [&stray](const QString &key) { return stray.contains(key); });
}

StrayIdentifiers::Removed StrayIdentifiers::remove(Library &library, std::span<const SpaceOperation> ops, bool dryRun) {
    const StrayIdentifiers strays(ops);
    if (strays.m_shared.isEmpty())
        return {};
    Removed said;
    const Library::Batch batch(library);
    for (const Book &book : library.books()) {
        const QString gid = Spaces::bookId(library, book.id);
        const QString standsFor = strays.m_canonical.value(gid, gid);
        if (!strays.m_shared.contains(standsFor))
            continue;
        const QSet<QString> stray = strays.of(book, gid, standsFor);
        const QSet<QString> doubtful = strays.doubtful(book, gid, standsFor);
        for (auto it = book.identifiers.begin(); it != book.identifiers.end(); ++it) {
            const QString value = it.value().toVariant().toString();
            const QString named = book.meta.title + u": "_s + it.key() + u' ' + value;
            if (isStray(doubtful, it.key(), value))
                said.doubtful << named;
            if (!isStray(stray, it.key(), value))
                continue;
            said.removed << named;
            if (!dryRun)
                library.setIdentifier(book.id, it.key(), {});
        }
    }
    return said;
}
