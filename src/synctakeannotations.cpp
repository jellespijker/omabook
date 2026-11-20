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
[[nodiscard]] Annotation annotationFrom(const ReducedAnnotation &reduced, qint64 id) {
    return {.id = id,
            .kind = fieldOf(reduced, u"kind"_s),
            .section = fieldOf(reduced, u"section"_s).toInt(),
            .fraction = fieldOf(reduced, u"fraction"_s).toDouble(),
            .text = fieldOf(reduced, u"text"_s),
            .note = fieldOf(reduced, u"note"_s),
            .created = fieldOf(reduced, u"created"_s).toLongLong()};
}

[[nodiscard]] bool unchanged(const Row &row, qint64 book, const Annotation &log) {
    const Annotation &here = row.annotation;
    return std::tie(row.book, here.kind, here.section, here.fraction, here.text, here.note, here.created)
           == std::tie(book, log.kind, log.section, log.fraction, log.text, log.note, log.created);
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
