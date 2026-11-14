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
    QString why;         // the model's reason, when it gave one; for a duplicate, why it is thought one
    // A duplicate found on weaker evidence (the titles only agree once a
    // subtitle is left off): shown for the person to judge, and left out of
    // a blind "apply everything".
    bool unsure = false;
};

namespace Curation {

// "Weis, Margaret" and "margaret  weis" are both "Margaret Weis".
QString tidyAuthor(const QString &name);

// What two spellings of a name share: the tidied name's letters and digits,
// lower case, accents dropped. "Sanderson, Brandon" and "brandon sanderson"
// have one key.
QString authorKey(const QString &name);
// What two spellings of one title share. Set aside: case, accents and
// punctuation; notes in brackets at the end ("(Mistborn #5)", "[Kindle
// Edition]"); a leading article ("The", "A", "An", "De", "Het", "Een", or
// one moved to the end: "Hobbit, The"); and a series number in front with a
// separator ("5- ", "06 - ", "#5 ", "Book 5: "). A number that is part of the
// title ("Mistborn 2", "1984", "2001: A Space Odyssey") stays.
QString titleKey(const QString &title);

// Books that are the same book twice: the same title (see titleKey) by the
// same author (any author of one is one of the other's, however spelled,
// "B. Sanderson" for "Brandon Sanderson" too). Not the same book: two
// different numbers in a series, an omnibus beside one of its parts, or the
// same title by someone else. Titles that only agree once a subtitle is left
// off ("The Bands of Mourning: A Mistborn Novel", "The Bands of Mourning")
// are proposed as unsure, when the subtitle describes the book ("A Mistborn
// Novel", a long non-fiction one) and what is left is not a series' name:
// "Mistborn: The Well of Ascension" and "Legion: Skin Deep" are books of
// their own. The copy with the most to it is kept
// and the others fold into it.
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
