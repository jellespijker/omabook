#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <span>

#include "spacereplay.h"

class Library;

// The same-as links of books that the change log may follow (#154, #158).
//
// A link (Replication::link) says two computers' ids name one book, and only
// ever grows. One made by a rule that was wrong, such as two books to read
// paired by the same "goodreads:synced" bookkeeping, or by a Calibre column
// two books share, folded different books into one, and a sync then gave each
// of them the title, authors and ISBN of another. Such a link stays in the
// log, so it is judged where it is followed: a link between two books' files'
// ids ("b:"), made by a file with the same content, holds; any other holds
// unless the books' titles, as the log said them when the link was made,
// disagree (BookIdentity::titlesAgree), or the log says the two are not one
// book (a "not_same_as", which record() writes). The judgement is the log's
// alone, so every computer that holds the same operations judges alike.
namespace BookLinks {

// `ops` without the same-as links of books that are not the same book.
[[nodiscard]] QList<SpaceOperation> trusted(std::span<const SpaceOperation> ops);

// The pairs of ids the log says are not one book, each as "smaller|larger".
[[nodiscard]] QSet<QString> apart(std::span<const SpaceOperation> ops);
[[nodiscard]] QString pairOf(const QString &one, const QString &other);

// Of the books that `trustedOps` (trusted()) fold together, the ids they
// stand for (SpaceReducer::sameAs) of those paired on less than their global
// id: by a link between ids other than two files' (an identifier, a title).
// A sync never changes the title, authors or ISBN of such a book by itself.
[[nodiscard]] QSet<QString> weaklyPaired(std::span<const SpaceOperation> trustedOps);

// Writes a "not_same_as" for each link of `ops` that trusted() leaves out and
// the log doesn't say so of yet: the titles it was judged by go once
// compaction drops them, and the link must stay dropped. The repair 0.2.2
// makes once of a library's log, and each sync of what came in. Answers each
// pair it wrote, as "<one title> / <the other>".
QStringList record(const Library &library, std::span<const SpaceOperation> ops);

// The way out the person takes of a link that holds by the rules above and is
// still wrong (#160): one an older Omabook made after it had already given one
// book the other's title, so their titles agreed then. The links between a
// book titled `title` (case set aside, on either computer) and one whose
// title disagrees with it, each as the log says its title now; never one
// between two files' ids. Each is recorded as not one book (a "not_same_as",
// as record() writes them) unless `dryRun`. Answers each pair, as
// "<one title> / <the other>"; none when no such link is there.
QStringList unlink(const Library &library, std::span<const SpaceOperation> ops, const QString &title, bool dryRun);

} // namespace BookLinks
