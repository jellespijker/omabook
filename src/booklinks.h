#pragma once

#include <QList>
#include <span>

#include "spacereplay.h"

// The same-as links of books that the change log may follow (#154).
//
// A link (Replication::link) says two computers' ids name one book, and only
// ever grows. One made by a rule that was wrong, such as two books to read
// paired by the same "goodreads:synced" bookkeeping, folded different books
// into one, and a sync then gave each of them the details of another. Such a
// link stays in the log, so it is judged where it is followed: a link between
// two books' files' ids ("b:"), made by a file with the same content, holds;
// one with a book named otherwise holds while the two books share an entry
// key (Library::entryKeys) as their details in the log say them now, or while
// the log says nothing of one of them. The judgement is the log's alone, so
// every computer that holds the same operations judges alike.
namespace BookLinks {

// `ops` without the same-as links of books that are not the same book.
[[nodiscard]] QList<SpaceOperation> trusted(std::span<const SpaceOperation> ops);

} // namespace BookLinks
