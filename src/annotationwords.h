#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "library.h"

// What the reader wrote in their books, found by its words (#115): a
// highlight's passage, a margin note, a bookmark's name, and the recognised
// text of a page's book ink, through the word index the annotations table
// keeps by its triggers (annotation_words, format 37, AnnotationSchema). Its
// words are folded as the graph's are (GraphIndex): case and the accents of
// Latin letters aside, a word found by its beginning ("light" finds
// "lighthouse").
//
// The reader's Find looks here as well as in the book's text, and the
// search's "notes:" (Book::Lookups) looks only here.
namespace AnnotationWords {

// The phrases of what was typed: each word, or the words in "quotes" together.
[[nodiscard]] QStringList phrases(const QString &typed);
// What the index is asked for them (FTS5's syntax): every phrase in the
// words of one annotation, its last word begun. None when no phrase has a
// letter or digit in it, which the index has no word for.
[[nodiscard]] std::optional<QString> query(const QStringList &phrases);

// A book's annotations that hold every phrase, in the order of the book.
[[nodiscard]] QList<Annotation> in(const Library &library, qint64 bookId, const QStringList &phrases);
// The books with an annotation that holds every phrase.
[[nodiscard]] QSet<qint64> books(const Library &library, const QStringList &phrases);
// The same of the library in the folder `root`, asked by a connection of its
// own that only reads: for the shelf, which holds no library. None when it
// can't be read.
[[nodiscard]] QSet<qint64> books(const QString &root, const QStringList &phrases);

// What Book::matches is given for "notes:": whether a value (a word, or
// words that were quoted) is in what was written in a book, each value asked
// of the index once and remembered for as long as the answer is kept. The
// first must not outlive the library.
[[nodiscard]] Book::Noted noted(const Library &library);
[[nodiscard]] Book::Noted noted(const QString &root);

// What the lookups above are made of, for the other word indexes too (BookWords):
// a lookup that asks `books` once for each value, and remembers; and the
// numbers a statement with one value bound answers of the library in the
// folder `root`, by a connection of its own that only reads (none when it
// can't be read).
[[nodiscard]] Book::Noted remembering(std::function<QSet<qint64>(const QString &)> books);
[[nodiscard]] QSet<qint64> numbersIn(const QString &root, const QString &statement, const QString &value);

} // namespace AnnotationWords
