#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "ai.h"
#include "bookmeta.h"
#include "library.h"
#include "metadatasource.h"

// What Omabook says to the model, kept in one place and free of any window
// or network, so the wording can be read, tested and changed on its own.
namespace Prompts {

// The most book text sent with one question. Models take far more than a
// novel, so this only guards against a runaway file; when it cuts, the part
// nearest the reader's place is kept and `trimmed` says so.
constexpr qsizetype maximumExcerpt = 1500000;

struct Excerpt {
    QString text;
    bool trimmed = false;
};
Excerpt fit(const QString &text);

// The standing instructions for the librarian of the whole library, who
// answers questions about what it holds.
QString librarian();
// The standing instructions for the reading companion of one book.
// `earlier` is what was noted in the books before it in its series, which
// the reader has read too.
QString companion(const BookMeta &book, const QList<SeriesNotes> &earlier = {});
// Those notes, as the model is given them; empty when there are none.
QString seriesSoFar(const QList<SeriesNotes> &earlier);
// The first message of a conversation: what the reader has in front of them
// (and has selected, if anything), then their question.
// `known` is the part of the book's knowledge graph the question touches (see
// GraphContext::forQuestion), empty when there is none; `marks` the reader's
// own highlights and notes up to their place (AnnotationContext), likewise.
QString question(const QString &excerpt, const QString &selection, const QString &question, const QString &known = {},
                 const QString &marks = {});
// The reader's highlights and notes, as the companion is given them with a question.
QString readersMarks(const QString &marks);

// Asks who or what the reader selected is, from the paragraph it stands in
// and nothing else of the book: for a name the knowledge graph doesn't know.
QString identify(const QString &selection, const QString &paragraph);
// Asks for three short questions a reader might ask about what they
// selected, one to a line; `known` is what the graph holds of it, if anything.
AiRequest moreQuestions(const BookMeta &book, const QString &selection, const QString &paragraph, const QString &known);
// Those questions out of the answer: at most three, each a line ending in "?".
QStringList questionsIn(const QString &answer);

// Asks to bring the reader back up to speed. `notes` is what the model wrote
// the last time it was asked (empty the first time) and `since` is only what
// has been read after that, so a long book is never sent twice. The answer
// is both what the reader sees and the notes for the next time.
// `earlier` is what was noted in the earlier books of the series; it
// is sent the first time only, since after that the notes carry it.
QString catchUp(const QString &notes, const QString &since, const QList<SeriesNotes> &earlier = {});

// Asks who is who so far: the people and places that matter, as far as the
// reader has got and no further. Built up like a catch-up: from the notes
// of last time and only what was read since.
QString cast(const QString &notes, const QString &since, const QList<SeriesNotes> &earlier = {});

// The knowledge graph. The kinds of thing the model is asked to find in a
// book: character, place, group, concept, item, event, thread, theme, topic.
QStringList graphKinds();
// Asks what a part of a book adds to its knowledge graph. `known` is what
// the graph already holds that this part could touch, one thing to a line,
// and `text` is only the part read since the graph was last brought up to
// date, section by section; the answer is a JSON object GraphBuild::apply
// reads. The whole book is never sent twice.
// `elsewhere` is what the general graph holds from the reader's other books
// that this part could be about too, so that a theme or an idea two books
// share becomes one node and not two spellings of it.
AiRequest graphExtraction(const BookMeta &book, const QString &known, const QString &text, const QString &elsewhere = {});
// Asks which things of a book connect to things of the other books read:
// `own` and `others` are numbered lines (see GraphLinks), `already` the
// connections there are. The answer is a JSON object GraphLinks::apply reads.
AiRequest graphConnections(const BookMeta &book, const QString &own, const QString &others, const QString &already = {});
// A slice of a book's graph as the companion is given it with a question.
QString graphSlice(const QString &slice);
// What the graph holds of a book, standing in for the model's own notes the
// first time a catch-up or who-is-who is asked for.
QString graphNotes(const QString &digest);
// What the graph says across the library (themes, ideas, what was asked,
// what was read when), as the librarian is given it.
QString graphPicture(const QString &picture);

// Asks for a change to how Omabook looks or works, to be made by rewriting
// its interface files. `state` is those files and settings as they are now.
// The answer is a JSON object parseChange reads.
struct InterfaceState {
    QString keys;       // keys.json, or empty when there is none
    QString style;      // style.json, likewise
    QJsonObject settings; // the settings that may be changed, as they are
    QJsonObject colors; // the colors in force, by name
    QString commands;   // the commands keys can be given to, one per line
    QString themes;     // the themes style.json's "theme" can name, comma separated
    QString collections; // the library's collections, "name: rule" a line each
    QString tags;       // the library's tags met most, with how many books have each
};
struct InterfaceChange {
    bool valid = false;
    bool hasKeys = false, hasStyle = false;
    QJsonObject keys, style, settings;
    // Collections to make, or to give another rule: [{"name", "rule"}].
    QJsonArray collections;
    QString say;        // what was changed, for the user
};
AiRequest change(const InterfaceState &state, const QString &wish);
InterfaceChange parseChange(const QString &answer);

// Asks for a book's details from its file name, what is known, its opening
// pages and what the catalogs found, which the model weighs rather than
// trusts; the answer is a JSON object parseMetadata reads.
AiRequest metadata(const QString &fileName, const BookMeta &known, const QString &opening,
                   const QList<MetaCandidate> &found = {});
// The details in a model's answer, with its reason; empty where it gave no JSON.
// `found` is what the model was shown, for the cover it chose among them.
MetaCandidate parseMetadata(const QString &answer, const QList<MetaCandidate> &found = {});

} // namespace Prompts
