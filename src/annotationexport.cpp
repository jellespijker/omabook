#include "annotationexport.h"

#include <QDateTime>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>
#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <tuple>

#include "annotationanchors.h"
#include "bookink.h"
#include "changelog.h"
#include "collation.h"
#include "document.h"
#include "formats.h"
#include "highlightcolors.h"
#include "highlights.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto marginNote = "margin-note";

// Whether two annotations are on the same words: a highlight and its note.
[[nodiscard]] bool samePassage(const Annotation &one, const Annotation &other) {
    return one.place == other.place && one.passage().quote.simplified() == other.passage().quote.simplified();
}

// A passage of a book as it is exported: its words, the notes on them, its
// colour when it is a highlight, and where it is.
struct Entry {
    Annotation at;
    QString quote;
    QStringList notes;
    bool highlighted = false;
};

// The book's passages, each once: a highlight with the margin notes on its
// words, and the margin notes on words that are not highlighted.
[[nodiscard]] QList<Entry> entriesOf(const QList<Annotation> &annotations) {
    QList<Entry> entries;
    for (const Annotation &annotation : annotations) {
        if (!annotation.hasPassage() || (annotation.kind != Highlights::kind && annotation.kind != marginNote))
            continue;
        const QString note = annotation.note.trimmed();
        const bool highlight = annotation.kind == Highlights::kind;
        // A note joins the highlight on its words, whichever came first.
        const auto same = std::ranges::find_if(
            entries, [&](const Entry &entry) { return samePassage(entry.at, annotation) && !(highlight && entry.highlighted); });
        if (same != entries.end()) {
            if (highlight) {
                same->at = annotation;
                same->highlighted = true;
            }
            if (!note.isEmpty())
                same->notes << note;
            continue;
        }
        entries.append({.at = annotation,
                        .quote = annotation.passage().quote.simplified(),
                        .notes = note.isEmpty() ? QStringList() : QStringList{note},
                        .highlighted = highlight});
    }
    return entries;
}

[[nodiscard]] QString sectionName(const AnnotationExport::Marked &book, int section) {
    const QString named = book.sections.value(section);
    return named.isEmpty() ? u"Section %1"_s.arg(section + 1) : named;
}

[[nodiscard]] QString colorLine(const Annotation &highlight, const QVariantMap &colorNames) {
    const QString key = HighlightColors::of(highlight.color).key;
    const QString named = colorNames.value(key).toString().trimmed();
    return "*" + key + (named.isEmpty() ? QString() : " · " + named) + "*";
}

// The lines of one passage: the words quoted, their notes, their colour.
[[nodiscard]] QStringList passageLines(const Entry &entry, const QVariantMap &colorNames) {
    QStringList lines;
    for (const QString &line : entry.quote.split('\n'))
        lines << "> " + line;
    lines << QString();
    for (const QString &note : entry.notes)
        lines << note << QString();
    if (entry.highlighted)
        lines << colorLine(entry.at, colorNames) << QString();
    return lines;
}

// Markdown's lines for a book: a heading where the section changes, and
// its passages, bookmarks and ink in the order of the book.
[[nodiscard]] QStringList bookLines(const AnnotationExport::Marked &book, const QVariantMap &colorNames) {
    QStringList lines{"# " + book.title};
    if (!book.authors.isEmpty())
        lines << QString() << book.authors.join(", ");
    lines << QString();
    struct Placed {
        BookPlace place;
        QStringList lines;
    };
    QList<Placed> found;
    QList<Placed> lost;
    for (const Entry &entry : entriesOf(book.annotations))
        (entry.at.lost ? lost : found).append({.place = entry.at.place, .lines = passageLines(entry, colorNames)});
    for (const Annotation &annotation : book.annotations) {
        if (annotation.kind == "bookmark")
            found.append({.place = annotation.place,
                          .lines = {"- Bookmark" + (annotation.text.trimmed().isEmpty() ? QString() : ": " + annotation.text.simplified()),
                                    QString()}});
        else if (annotation.kind == BookInk::kind)
            found.append({.place = annotation.place, .lines = {u"- Book ink on this page"_s, QString()}});
    }
    std::ranges::stable_sort(found, {}, &Placed::place);
    int section = -1;
    for (const Placed &placed : std::as_const(found)) {
        if (placed.place.section != section)
            lines << "## " + sectionName(book, placed.place.section) << QString();
        section = placed.place.section;
        lines << placed.lines;
    }
    if (!lost.isEmpty()) {
        lines << u"## Not found in this edition"_s << QString();
        for (const Placed &placed : std::as_const(lost))
            lines << placed.lines;
    }
    return lines;
}

// A field of a CSV row: quoted, its own quotes doubled (RFC 4180).
[[nodiscard]] QString field(const QString &value) { return "\"" + QString(value).replace(u'"', u"\"\""_s) + "\""; }

[[nodiscard]] QString dateOf(qint64 created) {
    return created > 0 ? QDateTime::fromSecsSinceEpoch(created, QTimeZone::UTC).toString(u"yyyy-MM-dd HH:mm:ss"_s) : QString();
}

// The names of a book's sections: its chapters' titles, or its pages.
[[nodiscard]] QHash<int, QString> sectionsOf(const Document &document) {
    QHash<int, QString> sections;
    if (document.kind() == Document::Pages) {
        for (int page = 0; page < document.count(); ++page)
            sections.insert(page, u"Page %1"_s.arg(page + 1));
        return sections;
    }
    // A chapter in two entries of the contents is called by the first.
    for (const TocEntry &entry : document.toc())
        if (!sections.contains(entry.section) && !entry.title.trimmed().isEmpty())
            sections.insert(entry.section, entry.title.simplified());
    return sections;
}

} // namespace

AnnotationExport::Marked AnnotationExport::of(Library &library, qint64 bookId, bool anchor) {
    Marked marked{.bookId = bookId, .title = {}, .authors = {}, .sections = {}, .annotations = {}};
    if (const auto book = library.book(bookId)) {
        marked.title = book->meta.title;
        marked.authors = book->meta.authors;
    }
    if (anchor)
        std::ignore = AnnotationAnchors::anchor(library, bookId); // A book without a file Omabook reads is exported as it is.
    if (const std::optional<FormatFile> file = AnnotationAnchors::readableFile(library, bookId))
        if (const std::unique_ptr<Document> document = Formats::open(file->path))
            marked.sections = sectionsOf(*document);
    marked.annotations = library.annotations(bookId);
    return marked;
}

QList<qint64> AnnotationExport::booksWithAnnotations(const Library &library) {
    QList<qint64> ids;
    for (QSqlQuery query = ChangeLog::query(library, "SELECT DISTINCT book_id FROM annotations ORDER BY book_id", {}); query.next();)
        ids << query.value(0).toLongLong();
    return ids;
}

QList<AnnotationExport::Marked> AnnotationExport::library(Library &library) {
    QList<Marked> books;
    for (const qint64 id : booksWithAnnotations(library))
        books << of(library, id, false);
    const QCollator collator = Collation::natural();
    std::ranges::stable_sort(books,
                             [&collator](const Marked &one, const Marked &other) { return collator.compare(one.title, other.title) < 0; });
    return books;
}

QString AnnotationExport::markdown(const QList<Marked> &books, const QVariantMap &colorNames) {
    QStringList lines;
    for (const Marked &book : books)
        lines << bookLines(book, colorNames);
    return lines.join('\n').trimmed() + '\n';
}

QByteArray AnnotationExport::readwise(const QList<Marked> &books) {
    QStringList rows{u"Highlight,Title,Author,URL,Note,Location,Date"_s};
    for (const Marked &book : books)
        for (const Entry &entry : entriesOf(book.annotations))
            rows << QStringList{field(entry.quote),
                                field(book.title),
                                field(book.authors.join(", ")),
                                field({}),
                                field(entry.notes.join("\n\n")),
                                QString::number(entry.at.place.section + 1),
                                field(dateOf(entry.at.created))}
                        .join(',');
    return (rows.join("\r\n") + "\r\n").toUtf8();
}
