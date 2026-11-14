#pragma once

#include <QList>
#include <QString>
#include <QUrl>

#include "ai.h"
#include "library.h"

// Looking after a whole library at once. A proposal is one change someone
// could want: nothing here touches the library until a proposal is applied,
// so the window and the command line can show them all and let the user
// choose. Duplicates and author spellings are found by comparing; series are
// the model's reading of the titles; details come from a look-up.
struct Proposal {
    QString kind;        // "duplicate", "author", "series" or "details"
    qint64 bookId = 0;   // the book it changes
    qint64 otherId = 0;  // duplicate: the book that is folded into bookId
    BookMeta meta;       // the book's details once it is applied (not for duplicates)
    QUrl coverUrl;       // details: a cover to fetch
    QString why;         // the model's reason, when it gave one
};

namespace Curation {

// "Weis, Margaret" and "margaret  weis" are both "Margaret Weis".
QString tidyAuthor(const QString &name);

// Books that are the same book twice: the same title by the same author.
// The copy with the most to it is kept and the others fold into it.
QList<Proposal> duplicates(const QList<Book> &books);
// Authors written "Last, First", or one author spelled several ways.
QList<Proposal> authorSpellings(const QList<Book> &books);

// Asks which books belong to a series, from their titles and authors alone.
AiRequest seriesRequest(const QList<Book> &books);
QList<Proposal> seriesFrom(const QString &answer, const QList<Book> &books);

// Asks a question of the library: what it holds, what each book is about,
// and what the reader has made of the ones they started.
struct BookNotes {
    Book book;
    QString recap;
};
// `graph` is what the knowledge graph holds across the books (see
// GraphContext::picture), empty when there is nothing or it is off.
AiRequest libraryQuestion(const QList<BookNotes> &books, const QString &question, const QString &graph = {});

// Asks what to read next: books the reader doesn't have, chosen from what
// they read (with their stars and when), are reading and want to read,
// whether those are files here, paper books, or books only known of from a
// reading site. A large library is summed up rather than listed: counts,
// the authors and tags met most, the most telling books read, and only
// titles for the rest. The model answers as JSON, so each suggestion can be
// checked against a reading site before it is shown.
AiRequest suggestionRequest(const QList<BookNotes> &books, const QString &graph = {});

struct Suggestion {
    QString title;
    QString author;
    QString why;
};
struct Suggestions {
    QList<Suggestion> books;
    QString first; // the unread book already here to pick up first, in the model's words
};
// What the model suggested; no books when it answered in prose instead.
Suggestions suggestionsFrom(const QString &answer);

// A book's details once a proposal is applied: only what the proposal is
// about changes, so two proposals for one book don't undo each other.
BookMeta after(const BookMeta &now, const Proposal &proposal);
bool apply(Library &library, const Proposal &proposal, QString *error = nullptr);

} // namespace Curation
