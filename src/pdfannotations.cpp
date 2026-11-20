#include "pdfannotations.h"

#include <QDir>
#include <QFileInfo>
#include <QRectF>
#include <QSizeF>
#include <algorithm>
#include <memory>
#include <poppler-annotation.h>
#include <poppler-qt6.h>
#include <ranges>

#include "annotationanchors.h"
#include "document.h"
#include "formats.h"
#include "highlightcolors.h"
#include "highlights.h"
#include "pagetext.h"

using namespace Qt::StringLiterals;

namespace {

// How big a margin note's mark is, as a share of the page's width.
constexpr double noteSide = 0.03;

// Where a passage's words stand on its page, a rectangle to a line, as a
// share of the page's width and height; none when they aren't on it.
[[nodiscard]] QList<QRectF> rectsOf(const Document &document, const Annotation &annotation) {
    const int page = annotation.place.section;
    const auto span = annotation.passage().spanIn(document.text(page));
    if (!span)
        return {};
    return PageText::rects(document.words(page), static_cast<int>(span->first), static_cast<int>(span->second), {1, 1});
}

[[nodiscard]] QRectF united(const QList<QRectF> &rects) {
    return std::ranges::fold_left(rects, QRectF(), [](const QRectF &all, const QRectF &one) { return all.united(one); });
}

void style(Poppler::Annotation &annotation, const QColor &color, double width = 1) {
    Poppler::Annotation::Style look;
    look.setColor(color);
    look.setWidth(width);
    annotation.setStyle(look);
}

// A highlight over its words, a quadrilateral a line, clockwise from the top left.
void addHighlight(Poppler::Page &page, const Annotation &highlight, const QList<QRectF> &rects) {
    Poppler::HighlightAnnotation made;
    made.setHighlightType(Poppler::HighlightAnnotation::Highlight);
    QList<Poppler::HighlightAnnotation::Quad> quads;
    for (const QRectF &rect : rects)
        quads.append({.points = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()},
                      .capStart = false,
                      .capEnd = false,
                      .feather = 0.1});
    made.setHighlightQuads(quads);
    made.setBoundary(united(rects));
    made.setContents(highlight.note.trimmed());
    style(made, HighlightColors::of(highlight.color).color);
    page.addAnnotation(&made);
}

// A note beside the start of its words, in the margin when there is room.
void addNote(Poppler::Page &page, const Annotation &note, const QList<QRectF> &rects, const QSizeF &size) {
    Poppler::TextAnnotation made(Poppler::TextAnnotation::Linked);
    const QRectF first = rects.first();
    const double down = noteSide * size.width() / size.height();
    made.setBoundary(QRectF(std::max(0.0, first.left() - noteSide), first.top(), noteSide, down));
    made.setTextIcon(u"Note"_s);
    made.setContents(note.note.trimmed());
    style(made, HighlightColors::all().first().color);
    page.addAnnotation(&made);
}

// A page's book ink: its points are in ink units across the page's width, and as many down.
void addInk(Poppler::Page &page, const BookInk::Page &ink, const QSizeF &size) {
    const double aspect = size.height() / size.width();
    for (const Ink::Stroke &stroke : ink.strokes) {
        if (stroke.points.empty())
            continue;
        Poppler::InkAnnotation made;
        QVector<QPointF> path;
        for (const Ink::Point &point : stroke.points)
            path << QPointF(point.at.x() / BookInk::pageUnits, point.at.y() / BookInk::pageUnits / aspect);
        made.setInkPaths({path});
        const QRectF area = BookInk::regionOf(ink.page, std::span(&stroke, 1), stroke.width).area;
        made.setBoundary(QRectF(area.x(), area.y() / aspect, area.width(), area.height() / aspect));
        style(made, stroke.color, stroke.width / BookInk::pageUnits * size.width());
        page.addAnnotation(&made);
    }
}

// Each highlight and margin note whose words are on its page, written in: how many.
int writePassages(const Poppler::Document &document, const Document &text, const QList<Annotation> &annotations) {
    int written = 0;
    for (const Annotation &annotation : annotations) {
        if (annotation.lost || !annotation.hasPassage() || (annotation.kind != Highlights::kind && annotation.kind != "margin-note"))
            continue;
        const std::unique_ptr<Poppler::Page> page = document.page(annotation.place.section);
        const QList<QRectF> rects = page ? rectsOf(text, annotation) : QList<QRectF>();
        if (rects.isEmpty())
            continue;
        if (annotation.kind == Highlights::kind)
            addHighlight(*page, annotation, rects);
        else
            addNote(*page, annotation, rects, page->pageSizeF());
        ++written;
    }
    return written;
}

// Each page's book ink written in: on how many pages.
int writeInk(const Poppler::Document &document, const QList<BookInk::Page> &ink) {
    int written = 0;
    for (const BookInk::Page &inked : ink) {
        const std::unique_ptr<Poppler::Page> page = document.page(inked.page);
        if (!page || page->pageSizeF().isEmpty() || inked.strokes.empty())
            continue;
        addInk(*page, inked, page->pageSizeF());
        ++written;
    }
    return written;
}

} // namespace

std::expected<int, QString> PdfAnnotations::write(const QString &pdf, const QList<Annotation> &annotations, const QList<BookInk::Page> &ink,
                                                  const QString &output) {
    if (QFileInfo(output).absoluteFilePath() == QFileInfo(pdf).absoluteFilePath())
        return std::unexpected(u"The annotated copy can't be written over the book's own file."_s);
    QString why;
    const std::unique_ptr<Document> text = Formats::open(pdf, &why);
    const std::unique_ptr<Poppler::Document> document = Poppler::Document::load(pdf);
    if (!text || text->kind() != Document::Pages || !document || document->isLocked())
        return std::unexpected(why.isEmpty() ? u"That isn't a PDF Omabook can open."_s : why);
    const int written = writePassages(*document, *text, annotations) + writeInk(*document, ink);
    const std::unique_ptr<Poppler::PDFConverter> converter = document->pdfConverter();
    converter->setOutputFileName(output);
    converter->setPDFOptions(converter->pdfOptions() | Poppler::PDFConverter::WithChanges);
    if (!converter->convert())
        return std::unexpected("The annotated PDF can't be written to " + QDir::toNativeSeparators(output) + ".");
    return written;
}

std::expected<int, QString> PdfAnnotations::write(Library &library, qint64 bookId, const QString &output) {
    const std::optional<FormatFile> file = AnnotationAnchors::readableFile(library, bookId);
    if (!file || file->format.compare("pdf"_L1, Qt::CaseInsensitive) != 0)
        return std::unexpected(u"Only a book read as a PDF can have its annotations written into it; export them as Markdown or "
                               "for Readwise instead."_s);
    // Its passages placed in that file first (ADR 0004).
    if (const auto anchored = AnnotationAnchors::anchor(library, bookId); !anchored)
        return std::unexpected(anchored.error());
    return write(file->path, library.annotations(bookId), BookInk::pages(library, bookId), output);
}
