#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtTypes>

#include "library.h"

// A book's annotations taken elsewhere (#86): as Markdown to read and keep,
// and as the CSV Readwise imports (its columns Highlight, Title, Author, URL,
// Note, Location and Date), for one book or the whole library. A highlight
// and a margin note on the same words are one entry, the words and the
// note; bookmarks and book ink, which have no words to quote, are in the
// Markdown only. The PDF with them written in is PdfAnnotations'.
namespace AnnotationExport {

// A book and its annotations, as an export says them.
struct Marked {
    qint64 bookId = 0;
    QString title;
    QStringList authors;
    // What each section is called (its chapter's title, or "Page 3"); a
    // section not named is "Section 4".
    QHash<int, QString> sections;
    QList<Annotation> annotations; // in the order of the book
};

// A book of the library with its annotations and its sections' names, read
// from its file when Omabook reads it. Its passages are found in that file
// first when `anchor` (ADR 0004): for a book, not for a sweep of the library.
[[nodiscard]] Marked of(Library &library, qint64 bookId, bool anchor = true);
// The books of the library that have annotations, by their numbers.
[[nodiscard]] QList<qint64> booksWithAnnotations(const Library &library);
// Every book of the library that has annotations, in the order of their titles.
[[nodiscard]] QList<Marked> library(Library &library);

// As Markdown: a heading for each book and under it for each section, each
// passage quoted with its note and its colour, by the name the reader gave
// it (`colorNames`, Settings::highlightNames) where they gave one; the lost
// ones at the end of their book.
[[nodiscard]] QString markdown(const QList<Marked> &books, const QVariantMap &colorNames = {});
// As Readwise's CSV, a row for each passage, its location the section counted from 1.
[[nodiscard]] QByteArray readwise(const QList<Marked> &books);

} // namespace AnnotationExport
