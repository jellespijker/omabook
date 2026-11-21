#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "library.h"

// Copies of one book on one computer that the change log knows as one book
// (#154): an entry without a file, as an import from Goodreads makes one, and
// the book's file, both with the same ISBN, that the one copy on another
// computer was linked to (Replication::link), the entry by its ISBN and the
// file by its content. The log folds their ids into one, so a sync decides
// that book's details once, from what both copies say, and gives both of them
// the details it settles on. The one that speaks for the book (its cover,
// what a dry run names it by) is the one with a file, before one without,
// then the one first added; each side picks it alike, of its own copies and
// of the other side's manifest.
namespace SyncCopies {

// `books`, one list for each book the log knows, each its copies (the ids the
// same-as links of `canonical` fold together), the one that speaks for it first.
[[nodiscard]] QList<QList<Book>> of(const Library &library, const QList<Book> &books, const QHash<QString, QString> &canonical);

// Whether `one` of the other side's manifest books speaks for a book before `other`.
[[nodiscard]] bool speaksBefore(const QJsonObject &one, const QJsonObject &other);

// Whether two books' details say the same in words (all but the cover).
[[nodiscard]] bool sameWords(const BookMeta &one, const BookMeta &other);

} // namespace SyncCopies
