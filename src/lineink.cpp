#include "lineink.h"

#include <QTextDocument>

#include <optional>

#include "annotationink.h"
#include "library.h"
#include "passagematch.h"

bool LineInk::isKind(QStringView kind) { return kind == QLatin1StringView(marginKind) || kind == QLatin1StringView(linesKind); }

QString LineInk::kindOf(InkLines::Where where) { return QString::fromLatin1(where == InkLines::Where::Margin ? marginKind : linesKind); }

InkLines::Where LineInk::whereOf(QStringView kind) {
    return kind == QLatin1StringView(marginKind) ? InkLines::Where::Margin : InkLines::Where::Lines;
}

QList<LineInk::Line> LineInk::lines(const Library &library, qint64 bookId) {
    QList<Line> found;
    for (const Annotation &annotation : library.annotations(bookId)) {
        if (!isKind(annotation.kind))
            continue;
        std::vector<Ink::Stroke> strokes = AnnotationInk::strokes(library, annotation.id);
        if (strokes.empty())
            continue;
        found << Line{.id = annotation.id,
                      .kind = annotation.kind,
                      .passage = annotation.passage(),
                      .lost = annotation.lost,
                      .strokes = std::move(strokes)};
    }
    return found;
}

std::optional<qint64> LineInk::make(Library &library, qint64 bookId, QStringView kind, const FoundPassage &passage, const QString &fileHash,
                                    std::vector<Ink::Stroke> strokes) {
    if (!isKind(kind) || strokes.empty() || PassageMatch::words(passage.passage.quote).isEmpty())
        return std::nullopt;
    Annotation line;
    line.kind = kind.toString();
    line.setPassage(passage.passage);
    // Placed in the file it is read from; one that wasn't is sought there when the book is opened next.
    if (passage.confidence > 0) {
        line.anchored = fileHash;
        line.confidence = passage.confidence;
    }
    const Library::Batch batch(library);
    const qint64 made = library.addAnnotation(bookId, line);
    if (made <= 0)
        return std::nullopt;
    if (!AnnotationInk::write(library, made, {.added = std::move(strokes), .removed = {}})) {
        library.removeAnnotation(made);
        return std::nullopt;
    }
    return made;
}

bool LineInk::write(Library &library, qint64 annotationId, const Ink::Change &change) {
    if (!AnnotationInk::write(library, annotationId, change))
        return false;
    // The ink is the annotation: with none left, there is none.
    if (!change.removed.isEmpty() && !AnnotationInk::has(library, annotationId))
        library.removeAnnotation(annotationId);
    return true;
}

QList<LineInk::Placed> LineInk::place(const QTextDocument &document, const QString &shown, const InkLines::Column &column,
                                      const QList<Line> &lines) {
    QList<Placed> placed;
    if (shown.isEmpty())
        return placed;
    // One index of the chapter's words for every line sought in it.
    std::optional<PassageMatch::Index> index;
    for (qsizetype at = 0; at < lines.size(); ++at) {
        const Line &line = lines.at(at);
        if (line.lost)
            continue;
        if (!index)
            index.emplace(shown);
        const auto match = PassageMatch::find(
            *index,
            {.quote = line.passage.quote, .before = line.passage.before, .after = line.passage.after, .near = line.passage.from.fraction});
        if (!match)
            continue;
        const InkLines::Anchor anchor{
            .where = whereOf(line.kind), .start = static_cast<int>(match->start), .end = static_cast<int>(match->end)};
        if (const std::optional<InkLines::Placing> placing = InkLines::Placing::of(document, column, anchor, line.strokes))
            placed << Placed{.index = at, .anchor = anchor, .placing = *placing};
    }
    return placed;
}
