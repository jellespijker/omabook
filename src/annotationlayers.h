#pragma once

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTypes>
#include <vector>

#include "bookplace.h"
#include "graph.h"
#include "inkstroke.h"

class Library;

// Other people's annotations on one's books (#85): an annotation layer.
// Of every space this library holds with its annotations part on (SpaceParts:
// a shared space as its writers switched it, a published one as its owner
// published it), each other member's highlights, margin notes and book ink,
// as SpaceAnnotations put them there, read from the space's operations and
// never written into one's own rows.
//
// A layer holds of a book only what its reader has reached, as `known` says
// (GraphContext::readerKnows, the one cut the graph's layers keep to): an
// annotation whose passage ends further on stays out until they get there,
// and nothing of a book they haven't begun, or don't have here. Each person
// has a name, as they go by in the space, and a colour of their own.
//
// What another person wrote is untrusted. Words are plain text, never markup:
// controls and the characters that turn text around are taken out, and a
// window shows them as plain text. What is larger than ADR 0002's limits is
// refused, never read: a value over PluginData::maxValueBytes, a change of a
// set of more than PluginData::maxItems strokes, and of an annotation no more
// than that many strokes in all are drawn.
namespace AnnotationLayers {

struct Annotation {
    QString id;   // its global id in the space
    QString kind; // "highlight", "margin-note" or "ink"
    qint64 book = 0;
    BookPlace place;
    BookPlace to; // where its passage ends; a page's ink, its page
    QString quote, before, after;
    QString note;  // a margin note's words, plain text
    QString color; // the highlight colour they chose (HighlightColors)
    qint64 created = 0;
    std::vector<Ink::Stroke> strokes;

    [[nodiscard]] Passage passage() const { return {.from = place, .to = to, .quote = quote, .before = before, .after = after}; }
};

// One person's annotations, of every space they share with this library.
struct Person {
    QString person;                // their person id
    QString name;                  // as they go by there; empty where none is known
    QStringList spaces;            // the spaces they are from
    QList<Annotation> annotations; // in the order of the book
};

struct Layers {
    QList<Person> people;  // by name, then id
    qsizetype refused = 0; // operations over the limits, left out
};

// Each other person's annotations on the book `bookId` (of every book, when 0)
// that `known` says its reader knows.
[[nodiscard]] Layers read(const Library &library, const GraphKnown &known, qint64 bookId = 0);
// The same as far as the library's reader has read (GraphContext::readerKnows).
[[nodiscard]] Layers read(const Library &library, qint64 bookId = 0);

// The colour a person's annotations are shown in: the same on every computer.
[[nodiscard]] QColor colorOf(const QString &person);
// What a person is called where they have no name: the start of their id.
[[nodiscard]] QString calledOf(const Person &person);

// Words as others wrote them, made plain: no control characters (but new
// lines and tabs), none that turn the text's direction around.
[[nodiscard]] QString plain(const QString &words);

// {person, name, color, spaces, annotations: [{id, kind, book, section,
// fraction, to: {section, fraction}, quote, note, color, strokes}]}, the
// strokes as how many there are.
[[nodiscard]] QJsonObject toJson(const Person &person);
[[nodiscard]] QJsonArray toJson(const Layers &layers);

} // namespace AnnotationLayers
