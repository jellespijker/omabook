#include "bookink.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <iterator>
#include <map>

#include "annotationink.h"
#include "changelog.h"
#include "failure.h"
#include "inkcodec.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto entity = "annotation"_L1;
constexpr auto setField = "set:strokes"_L1;

[[nodiscard]] QList<Annotation> inkOn(const Library &library, qint64 bookId, int page) {
    QList<Annotation> found;
    for (const Annotation &annotation : library.annotations(bookId, QString::fromLatin1(BookInk::kind)))
        if (annotation.place.section == page)
            found << annotation;
    return found;
}

// The strokes of one annotation, as every computer's changes of its set make them.
[[nodiscard]] std::vector<Ink::Stroke> strokesOf(const Library &library, qint64 annotationId) {
    return AnnotationInk::strokes(library, annotationId);
}

// The page's annotation for ink, made for its first stroke: its global id
// its book, kind and page, the same on every computer (Spaces::annotationGlobalId).
[[nodiscard]] std::expected<qint64, QString> makeAnnotation(Library &library, qint64 bookId, int page, const QString &fileHash) {
    const QString bookGid = Spaces::bookId(library, bookId);
    if (bookGid.isEmpty())
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(bookId)}).text());
    Annotation ink{.id = 0,
                   .kind = QString::fromLatin1(BookInk::kind),
                   .place = {.section = page, .fraction = 0},
                   .text = {},
                   .note = {},
                   .created = 0};
    ink.to = {.section = page, .fraction = 1};
    ink.anchored = fileHash;
    const qint64 made = library.addAnnotation(bookId, ink);
    if (made <= 0)
        return std::unexpected(Failure(Failure::Code::InkNotKept, {QString::number(page + 1)}).text());
    // Made again after its last stroke was erased, it is the one it was: no copy of it.
    Spaces::setGlobalId(library, QString(entity), made, Spaces::annotationGlobalId(bookGid, ink.kind, page, 0, {}, {}));
    return made;
}

} // namespace

QList<BookInk::Page> BookInk::pages(const Library &library, qint64 bookId) {
    std::map<int, Page> byPage;
    for (const Annotation &annotation : library.annotations(bookId, QString::fromLatin1(kind))) {
        std::vector<Ink::Stroke> strokes = strokesOf(library, annotation.id);
        if (strokes.empty())
            continue;
        Page &page = byPage[annotation.place.section];
        if (page.id == 0)
            page = {.id = annotation.id, .page = annotation.place.section, .strokes = {}};
        std::ranges::move(strokes, std::back_inserter(page.strokes));
    }
    QList<Page> pages;
    for (auto &[number, page] : byPage) {
        BookInk::inOrder(page.strokes);
        pages << std::move(page);
    }
    return pages;
}

std::vector<Ink::Stroke> BookInk::strokes(const Library &library, qint64 bookId, int page) {
    std::vector<Ink::Stroke> all;
    for (const Annotation &annotation : inkOn(library, bookId, page))
        std::ranges::move(strokesOf(library, annotation.id), std::back_inserter(all));
    BookInk::inOrder(all);
    return all;
}

std::expected<void, QString> BookInk::write(Library &library, qint64 bookId, int page, const Ink::Change &change, const QString &fileHash) {
    if (change.empty())
        return {};
    const std::vector<QJsonObject> batches = Ink::batches(change);
    const Library::Batch batch(library);
    QList<Annotation> on = inkOn(library, bookId, page);
    if (on.isEmpty()) {
        if (change.added.empty())
            return {};
        const auto made = makeAnnotation(library, bookId, page, fileHash);
        if (!made)
            return std::unexpected(made.error());
        on = inkOn(library, bookId, page);
    }
    const QString space = Spaces::personalSpace(library);
    for (const QJsonObject &one : batches) {
        // What is added goes on the page's first annotation; what is erased
        // goes from each that may hold it (two computers' first strokes, met).
        for (qsizetype index = 0; index < on.size(); ++index) {
            QJsonObject value = one;
            if (index > 0)
                value = {{"add", QJsonObject()}, {"remove", one.value("remove")}};
            if (index > 0 && value.value("remove").toArray().isEmpty())
                continue;
            ChangeLog::append(library, {.space = space,
                                        .entity = QString(entity),
                                        .id = Spaces::annotationId(library, on.at(index).id),
                                        .field = QString(setField),
                                        .value = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)),
                                        .hlc = {}});
        }
    }
    // Its last stroke erased, the page has no ink: its annotation goes, and is listed no more.
    for (const Annotation &annotation : std::as_const(on))
        if (!change.removed.isEmpty() && strokesOf(library, annotation.id).empty())
            library.removeAnnotation(annotation.id);
    return {};
}

void BookInk::inOrder(std::vector<Ink::Stroke> &strokes) {
    std::ranges::stable_sort(
        strokes, [](const Ink::Stroke &a, const Ink::Stroke &b) { return a.began != b.began ? a.began < b.began : a.id < b.id; });
}

PageRegion BookInk::regionOf(int page, std::span<const Ink::Stroke> strokes, double margin) {
    QRectF covered;
    for (const Ink::Stroke &stroke : strokes)
        covered = covered.united(Ink::bounds(stroke));
    if (covered.isNull())
        return {.page = page, .area = {}};
    covered.adjust(-margin, -margin, margin, margin);
    const QRectF area(covered.x() / pageUnits, covered.y() / pageUnits, covered.width() / pageUnits, covered.height() / pageUnits);
    // Within the page across, and below its top.
    return {.page = page, .area = area.intersected(QRectF(0, 0, 1, std::max(area.bottom(), 0.0)))};
}
