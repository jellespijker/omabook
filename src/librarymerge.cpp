#include "librarymerge.h"

#include <QImage>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <utility>

#include "trash.h"

using namespace Qt::StringLiterals;

namespace {

// Whether `kept` says all that `folded` does: nothing, or the same, or, where
// `more` may follow, the same and more (a date with its month and day).
[[nodiscard]] bool says(const QString &kept, const QString &folded, bool more = false) {
    const QString mine = kept.simplified();
    const QString theirs = folded.simplified();
    return theirs.isEmpty() || (more ? mine.startsWith(theirs, Qt::CaseInsensitive) : mine.compare(theirs, Qt::CaseInsensitive) == 0);
}

[[nodiscard]] QString digits(const QString &isbn) { return QString(isbn).remove(u'-').remove(u' '); }

[[nodiscard]] QStringList nameWords(const QString &name) {
    static const QRegularExpression between(uR"([\W_]+)"_s, QRegularExpression::UseUnicodePropertiesOption);
    return name.toCaseFolded().split(between, Qt::SkipEmptyParts);
}

// Whether an author of the kept book shares a whole word of this name, an
// initial aside: "B. Sanderson" and "Sanderson, Brandon" are named by "Brandon Sanderson".
[[nodiscard]] bool named(const QStringList &kept, const QString &author) {
    const QStringList words = nameWords(author);
    return std::ranges::any_of(kept, [&words](const QString &one) {
        return std::ranges::any_of(nameWords(one), [&words](const QString &word) { return word.size() > 1 && words.contains(word); });
    });
}

[[nodiscard]] bool keepsSeries(const BookMeta &kept, const BookMeta &folded) {
    if (folded.series.simplified().isEmpty() || kept.series.simplified().isEmpty())
        return true; // nothing to keep, or the kept book takes it
    return says(kept.series, folded.series)
           && (folded.seriesIndex <= 0 || kept.seriesIndex <= 0 || qFuzzyCompare(kept.seriesIndex, folded.seriesIndex));
}

// A merge takes this file of the other book into the one kept when it is a format
// that one lacks. An empty file is nothing to keep; it goes to the trash with its folder.
[[nodiscard]] std::expected<void, Failure> taken(Library &library, const Book &kept, const FormatFile &file) {
    if (kept.formats.contains(file.format) || file.size == 0)
        return {};
    return library.addFormat(kept.id, file.path);
}

} // namespace

BookMeta LibraryMerge::details(const BookMeta &kept, const BookMeta &folded) {
    BookMeta merged = kept;
    merged.fillFrom(folded);
    // A series' number goes with the series it was said of.
    if (merged.seriesIndex <= 0 && !folded.series.isEmpty() && says(merged.series, folded.series))
        merged.seriesIndex = folded.seriesIndex;
    for (const QString &tag : folded.tags)
        if (!merged.tags.contains(tag, Qt::CaseInsensitive))
            merged.tags << tag;
    merged.cover = {}; // covers are files: Library::merge carries them
    return merged;
}

QStringList LibraryMerge::dropped(const Book &kept, const Book &folded) {
    const BookMeta &mine = kept.meta;
    const BookMeta &theirs = folded.meta;
    const bool authors = mine.authors.isEmpty()
                         || std::ranges::all_of(theirs.authors, [&mine](const QString &author) { return named(mine.authors, author); });
    const std::array fields{
        std::pair{u"authors"_s, authors},
        std::pair{u"series"_s, keepsSeries(mine, theirs)},
        std::pair{u"publisher"_s, mine.publisher.isEmpty() || says(mine.publisher, theirs.publisher)},
        std::pair{u"published"_s, mine.published.isEmpty() || says(mine.published, theirs.published, true)},
        std::pair{u"language"_s, mine.language.isEmpty() || says(mine.language, theirs.language, true)},
        std::pair{u"isbn"_s, mine.isbn.isEmpty() || says(digits(mine.isbn), digits(theirs.isbn))},
        std::pair{u"description"_s, mine.description.isEmpty() || says(mine.description, theirs.description)},
        std::pair{u"rating"_s, kept.rating <= 0 || folded.rating <= 0 || kept.rating == folded.rating},
        std::pair{u"finished"_s, kept.finished.isEmpty() || says(kept.finished, folded.finished)},
    };
    QStringList names;
    for (const auto &[name, stays] : fields)
        if (!stays)
            names << name;
    return names;
}

Book LibraryMerge::after(const Book &kept, const Book &folded) {
    Book book = kept;
    book.meta = details(kept.meta, folded.meta);
    if (kept.rating <= 0)
        book.rating = folded.rating;
    if (kept.finished.isEmpty())
        book.finished = folded.finished;
    return book;
}

std::expected<void, QString> LibraryMerge::carry(Library &library, const Book &kept, const Book &folded) {
    for (const FormatFile &file : library.formatFiles(folded.id))
        if (const auto put = taken(library, kept, file); !put)
            return std::unexpected(put.error().text());
    if (!kept.hasCover && folded.hasCover)
        library.setCover(kept.id, QImage(library.coverPath(folded.id)));
    // Where the folded book's folder can't go to the trash, the fold stops here:
    // the book stays whole, and the kept one has only been given copies.
    if (folded.path != kept.path)
        if (const auto gone = Trash::folder(library.root(), library.root() + u'/' + folded.path); !gone)
            return std::unexpected(gone.error().text());
    const BookMeta merged = details(kept.meta, folded.meta);
    if (QString why; merged.toJson() != kept.meta.toJson() && !library.setMeta(kept.id, merged, &why))
        return std::unexpected(why);
    return {};
}
