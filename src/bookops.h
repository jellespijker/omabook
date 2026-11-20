#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

#include "ai.h"
#include "curation.h"
#include "document.h"
#include "library.h"
#include "metadatasource.h"

// What the command line and the MCP server both do with a book, each written
// once: the commands print the result, the tools hand it to an agent. Every
// function here waits for its answer, so none is for the window's thread.
namespace BookOps {

// The file of a book that Omabook reads itself; empty, with `error`, for a
// book with no such format (one to convert first, or one that isn't a file).
QString readableFile(const Library &library, qint64 id, QString *error);
// Opens a book in the library by its number, or any book file by its path.
std::unique_ptr<Document> open(const Library *library, const QString &numberOrFile, QString *error);

// Where words occur in a book: {"section", "snippet"} for each, at most
// `limit` of them (all when negative). `more` says whether any were left out.
QJsonArray find(const Document &document, const QString &words, int limit = -1, bool *more = nullptr);

// Asks the configured model one thing and waits for the whole answer, handing
// each piece to `piece` as it is written. False, with `error`, when there is
// no model or it failed.
// no model or it failed. `note` is given what the provider had to say
// although it answered: a model that was refused, and which answered instead.
bool ask(const AiRequest &request, QString *answer, QString *error, const std::function<void(const QString &)> &piece = {},
         QString *note = nullptr);
// A question about one book, with its text up to and including `lastSection`
// (all of it when negative) to go on. `trimmed` says the text was too long to
// send whole.
// `known` is what the knowledge graph holds on what the question names, and
// `marks` the reader's own highlights and notes up to their place (AnnotationContext).
AiRequest bookQuestion(const Document &document, const QString &question, int lastSection = -1, bool *trimmed = nullptr,
                       const QString &known = {}, const QString &marks = {});
// The reader's own highlights and notes in a book of the library, up to
// their place and that section (all of it when negative), for a question
// about it where the privacy rule lets them go (AnnotationContext); empty
// for none, no library or no such book.
QString readersMarks(const Library *library, qint64 bookId, int lastSection = -1);
// The library as the model is told of it: every book with the notes kept on it.
QList<Curation::BookNotes> booksWithNotes(const Library &library);
// Those of some of its books (a collection's).
QList<Curation::BookNotes> booksWithNotes(const Library &library, const QList<Book> &books);

// A book's details looked up with every metadata plugin that is on and, with
// `ai`, judged by the model, whose recommendation then leads.
struct LookUp {
    QList<MetaCandidate> candidates;
    QStringList problems; // sources that could not answer, and why
    QString error;        // why the look-up could not happen at all
};
LookUp lookUp(const Library &library, const Book &book, bool ai);
// A candidate as it is shown: its details, the fields it "adds" and the ones
// it "changes" with both values.
QJsonObject candidateJson(const Book &book, const MetaCandidate &candidate);
// A candidate's name that stays the same however a listing is ordered: its
// source and a hash of what it would give the book ("apple-books-1a2b3c4d").
QString candidateId(const MetaCandidate &candidate);
// The last listing of a book's candidates, kept for a while in the cache
// folder so that "candidate 1" means what was shown, and so that a candidate
// can be taken by its id without looking the book up again. `ai` says
// whether the model judged them, which changes the order.
struct Listing {
    bool ai = false;
    QList<MetaCandidate> candidates;
    QStringList ids; // candidateId of each, as listed
};
constexpr qint64 listingLife = 3600; // seconds a listing is kept to
void rememberListing(const Library &library, qint64 bookId, bool ai, const QList<MetaCandidate> &candidates);
std::optional<Listing> lastListing(const Library &library, qint64 bookId);
void forgetListing(const Library &library, qint64 bookId);
// Takes a candidate's details and, when it has one worth having, its cover.
bool applyCandidate(Library &library, qint64 id, const MetaCandidate &candidate, QString *error);

// Sets what a book is called elsewhere, scheme by scheme ("goodreads" →
// "28507"); an empty value forgets that scheme. All or nothing: a scheme
// that is empty, holds a space or "=", or is "generated" (which plugin made
// the book, set by that plugin alone when it makes it) refuses the lot.
[[nodiscard]] std::expected<void, QString> setIdentifiers(Library &library, qint64 id, const QList<QPair<QString, QString>> &identifiers,
                                                          std::optional<qint64> at = std::nullopt);

// Converts a book with Calibre's ebook-convert and adds the result to it.
// Empty `from` is the format the reader prefers. Returns the new file, or an
// empty string and `error`.
QString convert(Library &library, qint64 id, const QString &to, const QString &from, QString *error);

// What would tidy a library, found by comparing (and, with `series`, the
// proposals read from the model's answer), ordered so that applying them one
// after another works: merges last, since they take books away.
QList<Proposal> tidy(const QList<Book> &books, const QList<Proposal> &series = {});
// {"kind", "id", "title", ...}: "folds_in" for a duplicate, "authors" for an
// author's spelling, "series" and "series_index" for a series.
QJsonObject proposalJson(const Book &book, const Proposal &proposal);
// The proposal such an object describes, to apply with Curation::apply.
Proposal proposalFromJson(const QJsonObject &object);

} // namespace BookOps
