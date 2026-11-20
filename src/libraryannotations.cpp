#include <QDateTime>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>

#include "highlightcolors.h"
#include "library.h"

// A book's bookmarks and highlights, kept beside its other rows. They travel
// between one's own computers by the change log (#66), which takes in every
// write here (the annotations table's triggers, ChangeLogAnnotations).

namespace {

constexpr auto columns = "id, kind, section, fraction, text, note, created, to_section, to_fraction, quote, quote_before, quote_after, "
                         "anchored, confidence, lost, color, area";

} // namespace

Passage Annotation::passage() const {
    // A highlight from before passages kept its words as its text.
    return {.from = place,
            .to = std::max(place, to),
            .quote = quote.isEmpty() && kind == "highlight" ? text : quote,
            .before = before,
            .after = after};
}

void Annotation::setPassage(const Passage &passage) {
    place = passage.from;
    to = passage.to;
    quote = passage.quote;
    before = passage.before;
    after = passage.after;
}

bool Annotation::hasPassage() const { return !passage().quote.trimmed().isEmpty() && kind != "bookmark" && area.isEmpty(); }

std::optional<PageRegion> Annotation::region() const {
    return area.isEmpty() ? std::nullopt : std::optional(PageRegion{.page = place.section, .area = area});
}

Annotation Annotation::on(const QString &kind, const PageRegion &region, double aspect) {
    // Within the page across, and below its top: a part beside it is none.
    const QRectF area = region.area.intersected(QRectF(0, 0, 1, std::max(region.area.bottom(), 0.0)));
    Annotation made{.id = 0, .kind = kind, .place = PageRegion{.page = region.page, .area = area}.place(aspect), .text = {}, .note = {}};
    // Its place is where it starts, and the part says how far it goes.
    made.to = made.place;
    made.area = area;
    return made;
}

QJsonObject Annotation::toJson(QJsonObject alongside) const {
    for (const auto &[key, value] : {std::pair{"kind", QJsonValue(kind)}, std::pair{"text", QJsonValue(text)},
                                     std::pair{"note", QJsonValue(note)}, std::pair{"created", QJsonValue(created)}})
        alongside.insert(key, value);
    // A highlight says its colour by its name, the first's too.
    if (kind == "highlight")
        alongside.insert("color", HighlightColors::of(color).key);
    if (hasPassage()) {
        alongside.insert("passage", passage().toJson());
        alongside.insert("confidence", confidence);
        alongside.insert("lost", lost);
    }
    // One on a part of its page says which (#115).
    if (const std::optional<PageRegion> on = region())
        alongside.insert("region", on->toJson());
    return place.toJson(alongside);
}

Annotation Annotation::fromJson(const QJsonObject &object) {
    Annotation annotation{.id = 0,
                          .kind = object.value("kind").toString(),
                          .place = BookPlace::fromJson(object),
                          .text = object.value("text").toString(),
                          .note = object.value("note").toString(),
                          .created = object.value("created").toInteger()};
    annotation.to = annotation.place;
    annotation.color = HighlightColors::stored(object.value("color").toString());
    if (const QJsonObject passage = object.value("passage").toObject(); !passage.isEmpty()) {
        annotation.setPassage(Passage::fromJson(passage));
        annotation.confidence = object.value("confidence").toDouble(1);
        annotation.lost = object.value("lost").toBool();
    }
    if (const QJsonObject region = object.value("region").toObject(); !region.isEmpty()) {
        const PageRegion on = PageRegion::fromJson(region);
        annotation.area = on.area.isEmpty() ? QRectF() : on.area;
    }
    return annotation;
}

qint64 Library::addAnnotation(qint64 bookId, const Annotation &annotation) {
    Annotation made = annotation;
    made.id = 0;
    made.created = QDateTime::currentSecsSinceEpoch();
    return storeAnnotation(bookId, made).value_or(0);
}

std::optional<qint64> Library::storeAnnotation(qint64 bookId, const Annotation &annotation) {
    // A bookmark, or one given only where it starts, ends there too.
    const BookPlace to = std::max(annotation.place, annotation.to);
    QVariantList values{bookId,
                        annotation.kind,
                        annotation.place.section,
                        annotation.place.fraction,
                        annotation.text,
                        annotation.note,
                        annotation.created,
                        to.section,
                        to.fraction,
                        annotation.quote,
                        annotation.before,
                        annotation.after,
                        annotation.anchored,
                        annotation.confidence,
                        annotation.lost ? 1 : 0,
                        annotation.color,
                        PageRegion::areaText(annotation.area)};
    if (annotation.id <= 0) {
        const auto made = insert("INSERT INTO annotations (book_id, kind, section, fraction, text, note, created, to_section, to_fraction, "
                                 "quote, quote_before, quote_after, anchored, confidence, lost, color, area) "
                                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                 values);
        return made ? std::optional<qint64>(*made) : std::nullopt;
    }
    values << annotation.id;
    if (!exec("UPDATE annotations SET book_id = ?, kind = ?, section = ?, fraction = ?, text = ?, note = ?, created = ?, to_section = ?, "
              "to_fraction = ?, quote = ?, quote_before = ?, quote_after = ?, anchored = ?, confidence = ?, lost = ?, color = ?, area = ? "
              "WHERE id = ?",
              values))
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

QList<Annotation> Library::annotations(qint64 bookId, const QString &kind) const {
    QList<Annotation> annotations;
    for (QSqlQuery found = query(
             QString("SELECT %1 FROM annotations WHERE book_id = ? AND (IFNULL(?, '') = '' OR kind = ?) ORDER BY section, fraction, id")
                 .arg(columns),
             {bookId, kind, kind});
         found.next();)
        annotations.append({.id = found.value(0).toLongLong(),
                            .kind = found.value(1).toString(),
                            .place = {.section = found.value(2).toInt(), .fraction = found.value(3).toDouble()},
                            .text = found.value(4).toString(),
                            .note = found.value(5).toString(),
                            .created = found.value(6).toLongLong(),
                            // A row from before passages has none: it ends where it starts.
                            .to = std::max(BookPlace{.section = found.value(2).toInt(), .fraction = found.value(3).toDouble()},
                                           BookPlace{.section = found.value(7).toInt(), .fraction = found.value(8).toDouble()}),
                            .quote = found.value(9).toString(),
                            .before = found.value(10).toString(),
                            .after = found.value(11).toString(),
                            .anchored = found.value(12).toString(),
                            .confidence = found.value(13).toDouble(),
                            .lost = found.value(14).toBool(),
                            .color = found.value(15).toString(),
                            .area = PageRegion::areaOf(found.value(16).toString())});
    return annotations;
}
