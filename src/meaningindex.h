#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "bookplace.h"
#include "failure.h"

class Library;

// A book's text kept by what it means (#72): its stretches (TextStretches),
// each with the embedding a model gave it, in the library beside the book's
// rows (format 40). Like the tenants' tables it is never in the change log:
// it is worked out on each computer, by its own model, from the book's file,
// and made anew when that file changes. Only books asked for are kept here
// (or every book, by a setting the operations above read).
//
// - meaning_books, one row a book: whether it was asked for, the file (by its
//   hash) and the model its stretches were made from, and when.
// - meaning_stretches, its stretches: where each is, its words and its
//   vector, of length 1, so that how near two are is their dot product.
// - meaning_words, a full-text index (FTS5) of the stretches' words, folded as
//   the graph's are (GraphIndex), for the words of a search beside its meaning.
//
// A search reads the vectors of the books asked about and measures each: no
// index of vectors, which a few thousand stretches a book don't need.
namespace MeaningIndex {

using Vector = std::vector<float>;

// The format step's statements: the tables, their index and triggers.
[[nodiscard]] QStringList step();

// What is kept of a book.
struct State {
    bool wanted = false; // asked for, so kept up to date as its file changes
    QString hash;        // the file its stretches were made from
    QString model;       // and the model
    int dimensions = 0;
    int stretches = 0;
    qint64 made = 0; // when, in seconds since the epoch; 0 before it was
    [[nodiscard]] bool built() const noexcept { return made > 0; }
};
[[nodiscard]] std::optional<State> state(const Library &library, qint64 bookId);
// The books with a row here, asked for or made, by number.
[[nodiscard]] QList<qint64> books(const Library &library);
[[nodiscard]] QList<qint64> wanted(const Library &library);

// Asks for a book to be kept, or no longer: one not asked for is forgotten.
[[nodiscard]] std::expected<void, Failure> want(const Library &library, qint64 bookId, bool wanted);

// A stretch of a book with its vector, as it is stored and found.
struct Stretch {
    BookPlace from;
    BookPlace to;
    QString text;
    Vector vector;
};
// Puts a book's stretches in place of those it had, made from that file by
// that model, all at once: a search sees the old or the new, never part.
[[nodiscard]] std::expected<void, Failure> store(const Library &library, qint64 bookId, const QString &hash, const QString &model,
                                                 std::span<const Stretch> stretches);

// The vector made to length 1 (none stays none), and its bytes as stored.
void normalize(Vector &vector);
[[nodiscard]] QByteArray pack(const Vector &vector);
[[nodiscard]] Vector unpack(const QByteArray &bytes);

// A stretch found, and how: its place among those nearest in meaning and
// among those that hold the words (from 0; -1 when it isn't), and the two
// put together.
struct Hit {
    qint64 id = 0;
    qint64 bookId = 0;
    BookPlace from;
    BookPlace to;
    QString text;
    double nearness = 0; // the cosine of its vector and the question's
    int meaningRank = -1;
    int wordsRank = -1;
    double score = 0;
};

// Where to look: these books' stretches made by `model`, each kept only where
// `keeps` says of the book and the stretch's end (the spoiler cut, which is
// the caller's), at most `candidates` of each kind.
struct Scope {
    QSet<qint64> books;
    QString model;
    std::function<bool(qint64 bookId, const BookPlace &to)> keeps;
    int candidates = 50;
};

// The stretches nearest the question's vector (of length 1), nearest first.
[[nodiscard]] QList<Hit> nearest(const Library &library, const Vector &question, const Scope &scope);
// The stretches that hold every word (each found by its beginning, as the
// reader's notes are), the best first (BM25): the words as they were typed
// lift a stretch that has them over one only near in meaning. Every word,
// since the words of a question in plain language ("the", "do") are in
// every stretch and would otherwise rank them all alike.
[[nodiscard]] QList<Hit> worded(const Library &library, const QStringList &words, const Scope &scope);
// The two lists as one (reciprocal rank fusion): a stretch high in either
// comes high, one in both higher; at most `limit`, their words read.
[[nodiscard]] QList<Hit> fused(const Library &library, const QList<Hit> &meaning, const QList<Hit> &words, int limit);

} // namespace MeaningIndex
