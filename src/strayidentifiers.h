#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <span>

#include "spacereplay.h"

class Library;
struct Book;

// The identifiers a book took of another book that the log now keeps apart
// from it (#164, #166, ADR 0009).
//
// An older Omabook that took two books for one gave each the names elsewhere
// the other had, and a strong one (an ISBN, a Goodreads id, a Calibre uuid,
// BookIdentity::strongKeys) on the wrong book is what pairs it wrongly at the
// next first meeting. Once the log says the two are not one book (a
// "not_same_as", BookLinks), an identifier both had is stray on the one when
// it is the other's own and not this one's.
//
// A book's own: the key its global id is made of; its ISBN (a book here by
// the ISBN it has, one elsewhere by the one the log says it has now); and the
// key first recorded, as the log says, on a book whose title then agrees with
// its title now (the Goodreads id of its entry, its Calibre uuid). Which of
// two books had a key first says nothing of whose it is: an older Omabook
// copied it to the other then. When neither, or both, has it as its own, it
// is kept; one the other had first is named as kept (doubtful()).
//
// Only strong schemes: a custom column ("genre") or bookkeeping
// ("goodreads:synced") may be the same of many books, and names none.
class StrayIdentifiers {
public:
    // As the log `ops` says it, its same-as links followed as
    // BookLinks::trusted judges them.
    explicit StrayIdentifiers(std::span<const SpaceOperation> ops);

    // Of the keys (as BookIdentity::strongKeys writes them) `book` might have,
    // whose global id is `gid` and stands for `standsFor` (as the links fold
    // it), the ones that are a book's kept apart from it and not its own.
    [[nodiscard]] QSet<QString> of(const Book &book, const QString &gid, const QString &standsFor) const;
    // The keys it shares with a book kept apart from it, which that one had
    // first, and neither has as its own: kept, in doubt.
    [[nodiscard]] QSet<QString> doubtful(const Book &book, const QString &gid, const QString &standsFor) const;

    // Whether the identifier `scheme` = `value` is one of the keys `stray`.
    [[nodiscard]] static bool isStray(const QSet<QString> &stray, const QString &scheme, const QString &value);

    // What remove() did: each identifier removed, and each kept in doubt, as
    // "<title>: <scheme> <value>".
    struct Removed {
        QStringList removed;
        QStringList doubtful;
    };

    // Removes from the books of `library`, whose log is `ops`, each identifier
    // that is stray by it; with `dryRun` only says so.
    static Removed remove(Library &library, std::span<const SpaceOperation> ops, bool dryRun);

private:
    enum class Whose : char { Theirs, Doubtful };

    [[nodiscard]] QSet<QString> keys(const Book &book, const QString &gid, const QString &standsFor, Whose whose) const;
    [[nodiscard]] bool owns(const Book &book, const QString &gid, const QString &key) const;

    QHash<QString, QString> m_canonical;            // the id each id stands for, as the links fold them
    QHash<QString, QHash<QString, Whose>> m_shared; // by the id a book stands for, the keys it shares with one kept apart
    QHash<QString, QStringList> m_origin;           // by key, the titles of the books it was first recorded on
};
