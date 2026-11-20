#include "synctakeannotations.h"

#include <QList>
#include <tuple>

#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// One here: the book it is of, and what it says.
struct Row {
    qint64 book = 0;
    Annotation annotation;
};

[[nodiscard]] QHash<QString, Row> rowsHere(const Library &library, const QHash<QString, qint64> &books) {
    QHash<QString, Row> rows;
    for (const qint64 book : books)
        for (const Annotation &annotation : library.annotations(book))
            if (const QString gid = Spaces::annotationId(library, annotation.id); !gid.isEmpty())
                rows.insert(gid, {.book = book, .annotation = annotation});
    return rows;
}

[[nodiscard]] QString fieldOf(const ReducedAnnotation &reduced, const QString &field) { return reduced.fields.value(field).value; }

// As the log says it is, its number the row's here (0 when there is none).
// One from an Omabook before passages (#80) has none of a passage's fields:
// a bookmark's place, or a highlight's start and words.
[[nodiscard]] Annotation annotationFrom(const ReducedAnnotation &reduced, qint64 id) {
    const auto field = [&reduced](const QString &name) { return fieldOf(reduced, name); };
    const BookPlace place{.section = field(u"section"_s).toInt(), .fraction = field(u"fraction"_s).toDouble()};
    const bool hasEnd = reduced.fields.contains(u"to_section"_s);
    return {.id = id,
            .kind = field(u"kind"_s),
            .place = place,
            .text = field(u"text"_s),
            .note = field(u"note"_s),
            .created = field(u"created"_s).toLongLong(),
            .to = hasEnd ? BookPlace{.section = field(u"to_section"_s).toInt(), .fraction = field(u"to_fraction"_s).toDouble()} : place,
            .quote = field(u"quote"_s),
            .before = field(u"before"_s),
            .after = field(u"after"_s),
            .anchored = field(u"anchored"_s),
            .confidence = reduced.fields.contains(u"confidence"_s) ? field(u"confidence"_s).toDouble() : 1,
            .lost = field(u"lost"_s) == "1"_L1,
            .color = field(u"color"_s),
            .area = PageRegion::areaOf(field(u"area"_s))};
}

[[nodiscard]] bool unchanged(const Row &row, qint64 book, const Annotation &log) {
    const Annotation &here = row.annotation;
    return std::tie(row.book, here.kind, here.place, here.text, here.note, here.created)
               == std::tie(book, log.kind, log.place, log.text, log.note, log.created)
           && std::tie(here.to, here.quote, here.before, here.after, here.anchored, here.confidence, here.lost, here.color, here.area)
                  == std::tie(log.to, log.quote, log.before, log.after, log.anchored, log.confidence, log.lost, log.color, log.area);
}

} // namespace

int SyncTakeAnnotations::take(Library &library, const QMap<QString, ReducedAnnotation> &merged, const QHash<QString, qint64> &books,
                              const QHash<QString, QString> &same, bool dryRun) {
    const QHash<QString, Row> rows = rowsHere(library, books);
    // One write: what it brings goes into the log by the id it has (below), never one made of its content here.
    const Library::Batch batch(library);
    int taken = 0;
    for (auto it = merged.cbegin(); it != merged.cend(); ++it) {
        const auto row = rows.constFind(it.key());
        if (SpaceReducer::isRemoved(it->alive, it->removed)) {
            if (row == rows.cend())
                continue;
            ++taken;
            if (!dryRun)
                library.removeAnnotation(row->annotation.id);
            continue;
        }
        const QString bookGid = fieldOf(*it, u"book"_s);
        const qint64 book = books.value(same.value(bookGid, bookGid));
        const Annotation log = annotationFrom(*it, row == rows.cend() ? 0 : row->annotation.id);
        if (book <= 0 || log.kind.isEmpty() || (row != rows.cend() && unchanged(*row, book, log)))
            continue;
        ++taken;
        if (dryRun)
            continue;
        if (const std::optional<qint64> stored = library.storeAnnotation(book, log); stored && row == rows.cend())
            Spaces::setGlobalId(library, u"annotation"_s, *stored, it.key());
    }
    return taken;
}
