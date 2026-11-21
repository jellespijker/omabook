#pragma once

#include <QString>
#include <QtTypes>
#include <expected>

#include "bookplace.h"
#include "failure.h"

class Library;

// A margin note made a fact of the knowledge graph (#85): its words what is
// known of a thing, learned at the note's place in its book, so the graph
// shows it only to a reader who got there, as everything it learned in a
// book (GraphContext::reachOf). It is kept in the graph of the book's tenant
// (ADR 0003, #109), and goes wherever that graph goes. The note stays a note.
//
// The words may be one's own (a margin note, or a highlight's passage) or
// another person's, from an annotation layer (AnnotationLayers): theirs are
// plain text there already, and once a fact they are one's own.
namespace AnnotationFacts {

// What becomes a fact: the words, the book and the place they are at, and a
// few words of the passage to find the spot again.
struct Source {
    qint64 book = 0;
    BookPlace place;
    QString words;
    QString anchor;
};

// Of one's own annotation by its number, or of one in an annotation layer by
// its global id ("…" as AnnotationLayers gives it), as far as the reader read.
[[nodiscard]] std::expected<Source, Failure> of(const Library &library, const QString &annotation);

// The thing it becomes a fact of: one there is (by its number, or by its
// name in the book's graph), or one called so made of `kind`, met at the place.
struct Thing {
    QString which; // a number or a name
    QString kind;  // to make it when there is none of that name
};

struct Made {
    qint64 node = 0;
    qint64 fact = 0;
    bool madeNode = false;
};

[[nodiscard]] std::expected<Made, Failure> make(const Library &library, const Source &source, const Thing &thing);

} // namespace AnnotationFacts
