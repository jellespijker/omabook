#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>

#include "failure.h"
#include "library.h"

// The words of a plugin's own books (#136), for the shelf's search: what a
// notebook's recognised text and transcripts are, the text its plugin's
// format describes it with (BookDescription), which the search finds as it
// finds a book's details. Kept in the library (format 42) and never in the
// change log, as the meaning index is (ADR 0007): each computer reads it from
// its own file through the plugin, and reads it again when that file changes.
// So a book's text goes nowhere the book doesn't: never into a space, nor a
// private book's (#127) anywhere, and a tenant (ADR 0003) is the graph's,
// which this is not; the book's text is found wherever the book is.
//
// - book_texts, one row a book: the hash of the file its words came from.
// - book_words, a full-text index (FTS5, keeping no text of its own) of
//   those words by book, folded as the graph's are (GraphIndex): case and the
//   accents of Latin letters aside, a word found by its beginning.
//
// Books Omabook reads itself are not kept here: what the shelf finds them by
// is their details, as it always was.
namespace BookWords {

// The format step's statements: the tables and the trigger that keeps them together.
[[nodiscard]] QStringList step();

// The books with a file in one of these formats (extensions) whose words
// aren't kept here, or were read from another file.
[[nodiscard]] QList<qint64> stale(const Library &library, const QStringList &formats);
// Reads the words of a book from its file in the first of these formats it
// has (through Formats, so a plugin's format is asked by its plugin) and
// keeps them in place of those it had.
[[nodiscard]] std::expected<void, Failure> index(const Library &library, qint64 bookId, const QStringList &formats);
// Keeps a book's words, read from the file with this hash, in place of those it had.
[[nodiscard]] std::expected<void, Failure> store(const Library &library, qint64 bookId, const QString &hash, const QString &text);

// The books whose words hold every phrase (AnnotationWords::phrases).
[[nodiscard]] QSet<qint64> books(const Library &library, const QStringList &phrases);
// The same of the library in the folder `root`, asked by a connection of its
// own that only reads: for the shelf, which holds no library.
[[nodiscard]] QSet<qint64> books(const QString &root, const QStringList &phrases);

// What Book::matches is given for a book's words (Book::Lookups::written):
// each value asked of the index once and remembered for as long as the
// answer is kept. The first must not outlive the library.
[[nodiscard]] Book::Noted written(const Library &library);
[[nodiscard]] Book::Noted written(const QString &root);

} // namespace BookWords
