#include "annotationedits.h"

#include <QDateTime>
#include <algorithm>
#include <memory>

#include "annotationanchors.h"
#include "booktext.h"
#include "document.h"
#include "formats.h"
#include "highlightcolors.h"
#include "highlights.h"

using namespace Qt::StringLiterals;

namespace {

// The words wanted, placed in the book's own text: found near the place
// given, in the file the reader reads it from. None, with why, when the
// book has that file and the words aren't in it; unplaced (sought when the
// book is opened) when it has none.
[[nodiscard]] std::expected<Annotation, QString> onWords(const Library &library, qint64 bookId, Annotation annotation) {
    const Passage sought = annotation.passage();
    const std::optional<FormatFile> file = AnnotationAnchors::readableFile(library, bookId);
    if (!file)
        return annotation;
    const std::unique_ptr<Document> document = Formats::open(file->path);
    if (!document)
        return annotation;
    const std::optional<FoundPassage> found = sought.findAgain(BookText(*document));
    if (!found)
        return std::unexpected(u"Those words aren't in the book."_s);
    annotation.setPassage(found->passage);
    annotation.confidence = found->confidence;
    annotation.anchored = file->hash;
    return annotation;
}

[[nodiscard]] std::expected<Annotation, QString> made(const Library &library, qint64 bookId, const AnnotationEdits::Wanted &wanted,
                                                      const QVariantMap &names) {
    Annotation annotation{.id = 0, .kind = wanted.kind, .place = wanted.place, .text = {}, .note = wanted.note.trimmed()};
    annotation.to = wanted.place;
    if (wanted.kind == "bookmark") {
        annotation.text = wanted.words.simplified();
        return annotation;
    }
    if (wanted.kind != Highlights::kind && wanted.kind != AnnotationEdits::marginNote)
        return std::unexpected("There is no kind of annotation called " + wanted.kind + "; it is highlight, margin-note or bookmark.");
    if (wanted.words.trimmed().isEmpty())
        return std::unexpected(u"Give the words it is on."_s);
    if (wanted.kind == AnnotationEdits::marginNote && annotation.note.isEmpty())
        return std::unexpected(u"A margin note needs words of its own: give them as its note."_s);
    if (wanted.kind == Highlights::kind) {
        const auto color = AnnotationEdits::colorNamed(wanted.color, names);
        if (!color)
            return std::unexpected(color.error());
        annotation.color = HighlightColors::stored(*color);
    }
    annotation.setPassage({.from = wanted.place, .to = wanted.place, .quote = wanted.words.simplified(), .before = {}, .after = {}});
    auto placed = onWords(library, bookId, annotation);
    // A margin note is listed by the start of its words, as one made in the reader is.
    if (placed && wanted.kind == AnnotationEdits::marginNote)
        placed->text = placed->quote.simplified().left(200);
    return placed;
}

} // namespace

std::expected<QString, QString> AnnotationEdits::colorNamed(const QString &asked, const QVariantMap &names) {
    QString key = asked.trimmed().toLower();
    if (key.isEmpty())
        return HighlightColors::all().first().key;
    if (HighlightColors::known(key))
        return key;
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        if (it.value().toString().compare(asked.trimmed(), Qt::CaseInsensitive) == 0 && HighlightColors::known(it.key()))
            return it.key();
    return std::unexpected("There is no highlight colour called " + asked + ".");
}

std::optional<Annotation> AnnotationEdits::find(const Library &library, qint64 bookId, qint64 id) {
    const QList<Annotation> annotations = library.annotations(bookId);
    const auto found = std::ranges::find(annotations, id, &Annotation::id);
    return found == annotations.cend() ? std::nullopt : std::optional(*found);
}

std::expected<qint64, QString> AnnotationEdits::add(Library &library, qint64 bookId, const Wanted &wanted, const QVariantMap &names) {
    std::expected<Annotation, QString> annotation = made(library, bookId, wanted, names);
    if (!annotation)
        return std::unexpected(annotation.error());
    annotation->created = QDateTime::currentSecsSinceEpoch();
    const std::optional<qint64> kept = library.storeAnnotation(bookId, *annotation);
    if (!kept)
        return std::unexpected(u"The annotation can't be written."_s);
    return *kept;
}

std::expected<Annotation, QString> AnnotationEdits::change(Library &library, qint64 bookId, qint64 id, const Change &change,
                                                           const QVariantMap &names) {
    std::optional<Annotation> annotation = find(library, bookId, id);
    if (!annotation)
        return std::unexpected(QString("Book %1 has no annotation %2.").arg(bookId).arg(id));
    if (change.note) {
        if (annotation->kind == marginNote && change.note->trimmed().isEmpty())
            return std::unexpected(u"A margin note can't be left without words; remove it instead."_s);
        annotation->note = change.note->trimmed();
    }
    if (change.color) {
        if (annotation->kind != Highlights::kind)
            return std::unexpected(u"Only a highlight has a colour."_s);
        const auto color = colorNamed(*change.color, names);
        if (!color)
            return std::unexpected(color.error());
        annotation->color = HighlightColors::stored(*color);
    }
    if (change.name) {
        if (annotation->kind != "bookmark")
            return std::unexpected(u"Only a bookmark has a name; a passage's words are the book's."_s);
        annotation->text = change.name->simplified();
    }
    if (!library.storeAnnotation(bookId, *annotation))
        return std::unexpected(u"The annotation can't be written."_s);
    return *annotation;
}

std::expected<void, QString> AnnotationEdits::remove(Library &library, qint64 bookId, qint64 id) {
    if (!find(library, bookId, id))
        return std::unexpected(QString("Book %1 has no annotation %2.").arg(bookId).arg(id));
    library.removeAnnotation(id);
    return {};
}
