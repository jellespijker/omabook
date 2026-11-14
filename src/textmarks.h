#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>

#include "graph.h"

// Finding the things of a book's knowledge graph in its text, so that the
// people, places and ideas the graph holds can be marked where the page
// names them. Plain code on plain text: the window lays the marks onto the
// chapter it shows, a test runs it on a string.
//
// The rule for a name:
//
// - It is matched as whole words: "Pell" is not found in "Pellham", and the
//   words of a name may be apart by a space, a hyphen or an apostrophe, but
//   never by the end of a paragraph.
// - Capitals count. In the name of a person, a place or a group, and in any
//   name the graph writes in title case, a word written with a capital must
//   have one on the page: the character "Will" is not the "will" of "she
//   will", and the event "The Breaking" is not "the breaking of bread". A
//   leading "The", "A" or "An" may be either, since a sentence says "the
//   Tide Guild". Other names ("Tide-singing", a one-word idea) are found in
//   any case.
// - A one-word name that is also an everyday word ("Will", "Hope", "Rose",
//   "Mark") is marked only where it can't be that word: capitalised in the
//   middle of a sentence. It counts as an everyday word when it is in a short
//   list of such words or when the chapter itself uses it in lower case.
//   "Will smiled." at the start of a sentence is then left alone, which is
//   the price of never lighting up "Will you come?".
// - A character is also found by each part of their name ("Ilse" of "Ilse
//   Varga") when that part is three letters or more, not a title ("Captain",
//   "Brother", "Lady") and not shared with another thing of the book; but
//   not where it stands in another name the graph doesn't know (the "Varga"
//   of "Nerys Varga", before Nerys was met).
// - The longest name wins: "Nerys Varga" over "Varga", "The Upper City"
//   over "City". Two things known by the same name go to the one met first.
namespace TextMarks {

// A thing that can be found: a node of the graph, met at a place in the book.
struct Thing {
    qint64 id = 0;
    QString kind, name, summary;
    QStringList aliases;
    GraphPlace met;
};

// Where a thing is named in a text: from `start`, `length` characters.
struct Mark {
    qsizetype start = 0, length = 0;
    qint64 thing = 0;
    bool operator==(const Mark &) const = default;
};

// The kinds that are named in a story's text; questions, plot threads and
// themes are not, and the library's own nodes (a book, an author) aren't a
// book's things.
bool marked(const QString &kind);

// What a reader at `place` has met, as things: `met` is Graph::metIn, whose
// front up to the first thing met later is what is known there.
QList<Thing> known(const QList<std::pair<GraphNode, GraphPlace>> &met, const GraphPlace &place);
// How many of `met` a reader at `place` has met: the length of that front.
qsizetype reach(const QList<std::pair<GraphNode, GraphPlace>> &met, const GraphPlace &place);

// The words of a text, each by where it starts and how long it is: runs of
// letters and digits.
struct Word {
    qsizetype start = 0, length = 0;
};
QList<Word> words(QStringView text);

// Whether a word (in lower case) is one so common that a name spelled like
// it can't be told from it at the start of a sentence.
bool everyday(const QString &lower);

// The names of a set of things, ready to be looked for in any number of texts.
class Matcher {
public:
    Matcher() = default;
    explicit Matcher(const QList<Thing> &things);

    // Every place in `text` that names one of the things, in order, none overlapping.
    QList<Mark> match(QStringView text) const;
    // The thing these words are exactly a name of (as selected on the page,
    // in any case), or 0 when they name none.
    qint64 named(QStringView words) const;
    // A fingerprint of the names and things: two matchers with the same one
    // find the same marks in any text.
    size_t fingerprint() const noexcept { return m_fingerprint; }
    bool isEmpty() const noexcept { return m_first.isEmpty(); }
    qsizetype termCount() const noexcept { return m_terms; }

private:
    struct Term {
        QStringList words; // as the graph writes them
        QStringList folded;
        qint64 thing = 0;
        bool part = false; // a part of a character's name, not a whole name
        // Whether the capitals it is written with must be on the page: for a
        // proper name, and for any other name the graph writes in title
        // case ("The Breaking", but not "Tide-singing").
        bool strict = false;
    };
    // The terms by their first word that isn't an article, folded, the
    // longest first; and those whose first word is an article, by the word after it.
    QHash<QString, QList<Term>> m_first;
    // The kind of each thing, which says whether its capitals count.
    QHash<qint64, QString> m_kinds;
    qsizetype m_terms = 0;
    size_t m_fingerprint = 0;
};

// A question worth asking about a thing, made from what the graph holds up
// to the reader's place: its shape, and the names that fill it in. The
// window words it in the reader's language.
struct Suggestion {
    enum class Shape {
        Thread,   // "What does %1 have to do with %2?" (an open plot thread)
        Place,    // "Why is %1 at %2?"
        Relation, // "Why is %1 %3 %2?" ("Why is Aldo mentor of Ilse?"), for a relation ending in "of"
        Between,  // "How are %1 and %2 connected?"
        Before,   // "What happened to %1 before?" (met in an earlier chapter)
        Known,    // "What do we know of %1 so far?"
    };
    Shape shape = Shape::Known;
    QString first, second, relation;
    bool operator==(const Suggestion &) const = default;
};
// At most `most` of them for a thing, the most telling first: the threads it
// is part of, where it is, how it relates to others, what came before. Only
// edges of `view` (the book's graph up to the place) are used.
QList<Suggestion> suggestions(const GraphNode &thing, const GraphPlace &met, const GraphView &view, const GraphPlace &here,
                              int most = 3);

} // namespace TextMarks
