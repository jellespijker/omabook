#pragma once

#include <QList>
#include <QString>
#include <QtTypes>
#include <expected>

#include "bookink.h"
#include "library.h"

// A PDF book with its annotations written into it as standard PDF
// annotations (#86), so another PDF reader shows them: each highlight a
// highlight over its words in its colour, each margin note a note beside
// its words, and each page's book ink as ink. The book's own file is never
// changed; the copy goes where it is asked to. A lost annotation, whose words
// are not found on the page, and a bookmark are left out.
namespace PdfAnnotations {

// The book's PDF with its annotations, written to `output`: how many were
// written in, or why not (it has no PDF, or is read from another file, whose
// places its annotations are in).
[[nodiscard]] std::expected<int, QString> write(Library &library, qint64 bookId, const QString &output);

// The same of a PDF file and the annotations and ink given, their places
// those of its pages.
[[nodiscard]] std::expected<int, QString> write(const QString &pdf, const QList<Annotation> &annotations, const QList<BookInk::Page> &ink,
                                                const QString &output);

} // namespace PdfAnnotations
