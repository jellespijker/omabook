#include "annotationanchors.h"

#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <memory>
#include <ranges>
#include <tuple>

#include "booktext.h"
#include "changelog.h"
#include "document.h"
#include "formats.h"

namespace {

// Those of a book that are on a passage, found by its words; a bookmark is a place.
constexpr auto onPassage = "kind != 'bookmark' AND (quote != '' OR (kind = 'highlight' AND trim(text) != ''))";

// The annotation as it is once found again in that file, or lost in it.
[[nodiscard]] Annotation placed(const Annotation &annotation, const BookText &text, const QString &fileHash) {
    Annotation now = annotation;
    now.anchored = fileHash;
    if (const std::optional<FoundPassage> found = annotation.passage().findAgain(text)) {
        now.setPassage(found->passage);
        now.confidence = found->confidence;
        now.lost = false;
    } else {
        // Its passage stays as it was, to say what it was on and to be sought
        // again in another file.
        now.setPassage(annotation.passage());
        now.confidence = 0;
        now.lost = true;
    }
    return now;
}

} // namespace

bool AnnotationAnchors::needed(const Library &library, qint64 bookId, const QString &fileHash) {
    QSqlQuery query =
        ChangeLog::query(library, QString("SELECT 1 FROM annotations WHERE book_id = ? AND anchored != ? AND %1 LIMIT 1").arg(onPassage),
                         {bookId, fileHash});
    return query.next();
}

AnnotationAnchors::Summary AnnotationAnchors::anchor(Library &library, qint64 bookId, const BookText &text, const QString &fileHash) {
    Summary summary;
    QList<Annotation> changed;
    for (const Annotation &annotation : library.annotations(bookId)) {
        if (!annotation.hasPassage() || annotation.anchored == fileHash)
            continue;
        const Annotation now = placed(annotation, text, fileHash);
        if (now.lost)
            ++summary.lost;
        else if (now.passage() == annotation.passage() && !annotation.lost)
            ++summary.kept;
        else
            ++summary.moved;
        changed << now;
    }
    const Library::Batch batch(library);
    for (const Annotation &annotation : std::as_const(changed))
        std::ignore = library.storeAnnotation(bookId, annotation); // One that can't be written is sought again next time.
    return summary;
}

std::optional<FormatFile> AnnotationAnchors::readableFile(const Library &library, qint64 bookId) {
    const auto book = library.book(bookId);
    if (!book)
        return std::nullopt;
    const QList<FormatFile> files = library.formatFiles(bookId);
    for (const QString &format : book->formats) {
        if (!Formats::readable(format))
            continue;
        const auto file = std::ranges::find(files, format, &FormatFile::format);
        if (file == files.cend())
            continue;
        FormatFile readable = *file;
        readable.path = library.formatPath(bookId, format);
        return readable;
    }
    return std::nullopt;
}

std::expected<AnnotationAnchors::Summary, QString> AnnotationAnchors::anchor(Library &library, qint64 bookId) {
    const std::optional<FormatFile> file = readableFile(library, bookId);
    if (!file)
        return std::unexpected(QString("That book has no file Omabook reads."));
    if (!needed(library, bookId, file->hash))
        return Summary{};
    QString why;
    const std::unique_ptr<Document> document = Formats::open(file->path, &why);
    if (!document)
        return std::unexpected(why);
    return anchor(library, bookId, BookText(*document), file->hash);
}

QList<Annotation> AnnotationAnchors::lost(const Library &library, qint64 bookId) {
    return library.annotations(bookId) | std::views::filter(&Annotation::lost) | std::ranges::to<QList<Annotation>>();
}

bool AnnotationAnchors::placeAgain(Library &library, qint64 annotationId, const Passage &passage, const QString &fileHash) {
    QSqlQuery query = ChangeLog::query(library, "SELECT book_id FROM annotations WHERE id = ?", {annotationId});
    if (!query.next())
        return false;
    const qint64 bookId = query.value(0).toLongLong();
    query.finish();
    const QList<Annotation> annotations = library.annotations(bookId);
    const auto found = std::ranges::find(annotations, annotationId, &Annotation::id);
    if (found == annotations.cend())
        return false;
    Annotation placed = *found;
    placed.setPassage(passage);
    placed.anchored = fileHash;
    placed.confidence = 1;
    placed.lost = false;
    return library.storeAnnotation(bookId, placed).has_value();
}
