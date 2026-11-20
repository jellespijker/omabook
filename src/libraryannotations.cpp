#include <QDateTime>
#include <QSqlQuery>
#include <QVariant>

#include "library.h"

// A book's bookmarks and highlights, kept beside its other rows. They travel
// between one's own computers by the change log (#66), which takes in every
// write here (the annotations table's triggers, ChangeLogAnnotations).

qint64 Library::addAnnotation(qint64 bookId, const Annotation &annotation) {
    Annotation made = annotation;
    made.id = 0;
    made.created = QDateTime::currentSecsSinceEpoch();
    return storeAnnotation(bookId, made).value_or(0);
}

std::optional<qint64> Library::storeAnnotation(qint64 bookId, const Annotation &annotation) {
    const QVariantList values{bookId,          annotation.kind, annotation.section, annotation.fraction,
                              annotation.text, annotation.note, annotation.created};
    if (annotation.id <= 0) {
        const auto made =
            insert("INSERT INTO annotations (book_id, kind, section, fraction, text, note, created) VALUES (?, ?, ?, ?, ?, ?, ?)", values);
        return made ? std::optional<qint64>(*made) : std::nullopt;
    }
    QVariantList updated = values;
    updated << annotation.id;
    if (!exec("UPDATE annotations SET book_id = ?, kind = ?, section = ?, fraction = ?, text = ?, note = ?, created = ? WHERE id = ?",
              updated))
        return std::nullopt;
    return annotation.id;
}

bool Library::renameAnnotation(qint64 annotationId, const QString &text) {
    QSqlQuery found = query("SELECT 1 FROM annotations WHERE id = ?", {annotationId});
    if (!found.next())
        return false;
    found.finish();
    return exec("UPDATE annotations SET text = ? WHERE id = ?", {text.simplified(), annotationId});
}

void Library::removeAnnotation(qint64 annotationId) { exec("DELETE FROM annotations WHERE id = ?", {annotationId}); }

QList<Annotation> Library::annotations(qint64 bookId) const {
    QList<Annotation> annotations;
    for (QSqlQuery found = query("SELECT id, kind, section, fraction, text, note, created FROM annotations WHERE book_id = ? "
                                 "ORDER BY section, fraction",
                                 {bookId});
         found.next();)
        annotations.append({.id = found.value(0).toLongLong(),
                            .kind = found.value(1).toString(),
                            .section = found.value(2).toInt(),
                            .fraction = found.value(3).toDouble(),
                            .text = found.value(4).toString(),
                            .note = found.value(5).toString(),
                            .created = found.value(6).toLongLong()});
    return annotations;
}
