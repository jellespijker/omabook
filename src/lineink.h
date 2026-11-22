#pragma once

#include <QList>
#include <QString>
#include <QStringView>
#include <QtTypes>

#include <optional>
#include <vector>

#include "bookplace.h"
#include "inklines.h"
#include "inkstroke.h"

class Library;
class QTextDocument;

// Free ink on a flowing book (#152, ADR 0010): handwriting and drawing in the
// side margins and between the lines of an EPUB, a FictionBook, Markdown or a
// text, not on words selected first (that is a margin note, #90). The strokes
// beside one line are an annotation of their own, on a passage of that line
// (ADR 0004): of the kind "margin-ink" in a side margin, on the whole line;
// of the kind "line-ink" among the lines, on the words under or over them.
// So they are found again by their words when the book's file changes, go
// where the line goes when the text is laid out again (InkLines), and are
// listed, synced, carried into spaces and removed as every annotation is.
// Their strokes are a set of that annotation in the change log, as a page's
// book ink is (ADR 0006, AnnotationInk), in ink units of the line's height
// (InkLines::lineUnits). An annotation goes with its last stroke.
namespace LineInk {

inline constexpr auto marginKind = "margin-ink";
inline constexpr auto linesKind = "line-ink";

// Whether an annotation of this kind is free ink.
[[nodiscard]] bool isKind(QStringView kind);
[[nodiscard]] QString kindOf(InkLines::Where where);
[[nodiscard]] InkLines::Where whereOf(QStringView kind);

// The strokes beside one line, and what they are on.
struct Line {
    qint64 id = 0;
    QString kind;
    Passage passage;
    bool lost = false;
    std::vector<Ink::Stroke> strokes;
};

// The book's free ink, a line at a time in the order of the book; those
// whose words aren't in this edition too, as lost.
[[nodiscard]] QList<Line> lines(const Library &library, qint64 bookId);

// A line's first strokes, on a new annotation of that kind on the passage
// (placed in the file of `fileHash` when it was found there, `confidence`
// over 0): its number, none when it can't be kept.
[[nodiscard]] std::optional<qint64> make(Library &library, qint64 bookId, QStringView kind, const FoundPassage &passage,
                                         const QString &fileHash, std::vector<Ink::Stroke> strokes);
// A change of a line's strokes, kept in the change log in one write; the
// line's annotation goes with its last stroke. False when there is no such line.
bool write(Library &library, qint64 annotationId, const Ink::Change &change);

// Where lines stand on a chapter as it is laid out (a document showing their
// section, `shown` its plain text): each that is found there, with the place
// of its strokes; in the order given.
struct Placed {
    qsizetype index = 0; // of the line among those given
    InkLines::Anchor anchor;
    InkLines::Placing placing;
};
[[nodiscard]] QList<Placed> place(const QTextDocument &document, const QString &shown, const InkLines::Column &column,
                                  const QList<Line> &lines);

} // namespace LineInk
