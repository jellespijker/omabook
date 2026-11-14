#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

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
// `known` is what the knowledge graph holds on what the question names.
AiRequest bookQuestion(const Document &document, const QString &question, int lastSection = -1, bool *trimmed = nullptr,
                       const QString &known = {});
// The library as the model is told of it: every book with the notes kept on it.
QList<Curation::BookNotes> booksWithNotes(const Library &library);

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
// Takes a candidate's details and, when it has one worth having, its cover.
bool applyCandidate(Library &library, qint64 id, const MetaCandidate &candidate, QString *error);

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
