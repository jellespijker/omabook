#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <span>
#include <utility>

#include "spacereplay.h"

class Library;

// How a sync finds, here, the other computer's copy of a book it never paired
// before (#158, ADR 0009). Once paired, a book stays paired by its global id
// and is never paired again: only a book whose id stands for no book here is
// looked for, and only a book here that is paired with none of theirs is
// found.
//
// A book with a file is the one here with a file of the same content. One
// without a file is found by an identifier of a strong scheme
// (BookIdentity::strongKeys), and only one to one: a key that names two books
// on either side pairs nothing, and two keys that name two books here pair
// neither. And only with agreement: the two titles must agree
// (BookIdentity::titlesAgree); when they don't, the two are kept apart and
// said to be. Both sides find alike, so a dry run from either says the same.
namespace BookPairing {

// A book as the other side names it, for this side to find its copy.
struct Names {
    QString gid;
    QStringList hashes; // its files' hashes, and the ones they came in with (#74)
    QStringList keys;   // without a file, its strong keys no other book there has (keysAlone)
    QString title;
};

// What a sync would pair: each link as (the id here, theirs), and the books
// that share an identifier but not a title, as "<here> / <there> (<key>)".
struct Pairs {
    QList<std::pair<QString, QString>> links;
    QStringList apart;
};

// Of each book of `library` by its id, the strong keys no other book there has.
[[nodiscard]] QHash<qint64, QStringList> keysAlone(const Library &library);

// What pairing `theirs` with the books of `library`, whose log is `ops`, would
// link; `theirGids` are the ids of every book they hold, of which a book here
// paired with one is paired already.
[[nodiscard]] Pairs pairs(const Library &library, std::span<const SpaceOperation> ops, const QList<Names> &theirs,
                          const QSet<QString> &theirGids);

// The links of `pairs` as the operations this computer would write now, for
// a dry run, which writes none of them: stamped `at`, after every operation it
// has seen, as a sync's clock is once it has seen the other side's.
[[nodiscard]] QList<SpaceOperation> asOperations(const Library &library, const Pairs &pairs, qint64 at);

// Writes the links of `pairs` to the log; answers how many.
int record(const Library &library, const Pairs &pairs);

} // namespace BookPairing
