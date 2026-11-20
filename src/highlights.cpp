#include "highlights.h"

#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <ranges>

#include "booktext.h"
#include "changelog.h"
#include "highlightcolors.h"

QList<Annotation> Highlights::of(const Library &library, qint64 bookId, QStringView color) {
    const QString wanted = HighlightColors::stored(color);
    const auto chosen = [&](const Annotation &annotation) { return color.isEmpty() || annotation.color == wanted; };
    return library.annotations(bookId, kind) | std::views::filter(chosen) | std::ranges::to<QList<Annotation>>();
}

std::optional<std::pair<qint64, Annotation>> Highlights::find(const Library &library, qint64 id) {
    QSqlQuery query = ChangeLog::query(library, "SELECT book_id FROM annotations WHERE id = ?", {id});
    if (!query.next())
        return std::nullopt;
    const qint64 bookId = query.value(0).toLongLong();
    query.finish();
    const QList<Annotation> annotations = library.annotations(bookId);
    const auto found = std::ranges::find(annotations, id, &Annotation::id);
    if (found == annotations.cend())
        return std::nullopt;
    return std::pair{bookId, *found};
}

std::expected<qint64, QString> Highlights::make(Library &library, qint64 bookId, const Passage &passage, QStringView color,
                                                const BookText *text, const QString &fileHash) {
    if (passage.quote.trimmed().isEmpty())
        return std::unexpected(QString("There are no words to highlight."));
    if (!HighlightColors::known(color))
        return std::unexpected("There is no colour called " + color.toString() + ".");
    Annotation highlight{.id = 0, .kind = kind, .place = {}, .text = {}, .note = {}, .created = 0};
    highlight.setPassage(passage);
    highlight.color = HighlightColors::stored(color);
    if (text) {
        if (const std::optional<FoundPassage> found = passage.findAgain(*text)) {
            highlight.setPassage(found->passage);
            highlight.confidence = found->confidence;
            highlight.anchored = fileHash;
        }
    }
    const qint64 made = library.addAnnotation(bookId, highlight);
    if (made <= 0)
        return std::unexpected(QString("The highlight couldn't be kept."));
    return made;
}

bool Highlights::recolor(Library &library, qint64 id, QStringView color) {
    std::optional<std::pair<qint64, Annotation>> found = find(library, id);
    if (!found || found->second.kind != kind || !HighlightColors::known(color))
        return false;
    found->second.color = HighlightColors::stored(color);
    return library.storeAnnotation(found->first, found->second).has_value();
}
