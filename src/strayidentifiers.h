#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <span>

#include "spacereplay.h"

class Library;

// The identifiers a book took of another book that the log now keeps apart
// from it (#164, ADR 0009).
//
// An older Omabook that took two books for one gave each the names elsewhere
// the other had, and a strong one (an ISBN, a Goodreads id, a Calibre uuid,
// BookIdentity::strongKeys) on the wrong book is what pairs it wrongly at the
// next first meeting. Once the log says the two are not one book (a
// "not_same_as", BookLinks), an identifier both have that the other had first,
// as the log says when each had it, is the other's: it is stray here, a sync
// takes none of it, and it is removed (remove()) at upgrade and at each sync.
// Only strong schemes: a custom column ("genre") or bookkeeping
// ("goodreads:synced") may be the same of many books, and names none.
namespace StrayIdentifiers {

// Of each id the same-as links the log follows fold books into
// (SpaceReducer::sameAs of BookLinks::trusted), the strong keys it has of a
// book the log keeps apart from it, as BookIdentity::strongKeys writes them.
[[nodiscard]] QHash<QString, QSet<QString>> of(std::span<const SpaceOperation> ops);

// Whether the identifier `scheme` = `value` is one of the keys `stray`.
[[nodiscard]] bool isStray(const QSet<QString> &stray, const QString &scheme, const QString &value);

// Removes from the books of `library`, whose log is `ops`, each identifier
// that is stray by it; with `dryRun` only says so. Answers each, as
// "<title>: <scheme> <value>".
QStringList remove(Library &library, std::span<const SpaceOperation> ops, bool dryRun);

} // namespace StrayIdentifiers
