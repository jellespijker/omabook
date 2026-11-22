#include "bookrestore.h"

#include <QSet>
#include <algorithm>
#include <optional>
#include <ranges>
#include <vector>

#include "bookink.h"
#include "library.h"
#include "librarydiff.h"
#include "spaces.h"

namespace {

using namespace Qt::StringLiterals;

constexpr auto identifier = "identifiers."_L1;

// The details of `book` in `live` set to those of `earlier`: whether any changed.
[[nodiscard]] std::expected<bool, Failure> putDetailsBack(Library &live, const Book &book, const Book &earlier,
                                                          const QList<LibraryDiff::Field> &fields) {
    if (fields.isEmpty())
        return false;
    if (std::ranges::any_of(fields, [](const LibraryDiff::Field &field) { return !field.field.startsWith(identifier); })) {
        // The earlier details, without a cover: the one there stays.
        if (QString error; !live.setMeta(book.id, earlier.meta, &error))
            return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {error}));
    }
    for (const LibraryDiff::Field &field : fields)
        if (field.field.startsWith(identifier) && !live.setIdentifier(book.id, field.field.mid(identifier.size()), field.after))
            return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {field.field}));
    return true;
}

// What of an annotation a restore puts back: where it is, its words, note and colour.
[[nodiscard]] bool looksAlike(const Annotation &a, const Annotation &b) {
    return a.kind == b.kind && a.place == b.place && a.to == b.to && a.text == b.text && a.note == b.note && a.color == b.color
           && a.area == b.area;
}

// The strokes an ink page had that it lacks now, drawn on it again: how many.
[[nodiscard]] std::expected<int, Failure> putStrokesBack(Library &live, const Library &from, qint64 book, qint64 earlier,
                                                         const Annotation &page) {
    QSet<QString> there;
    for (const Ink::Stroke &stroke : BookInk::strokes(live, book, page.place.section))
        there.insert(stroke.id);
    std::vector<Ink::Stroke> missing;
    std::ranges::copy_if(BookInk::strokes(from, earlier, page.place.section), std::back_inserter(missing),
                         [&there](const Ink::Stroke &stroke) { return !there.contains(stroke.id); });
    if (missing.empty())
        return 0;
    const int count = static_cast<int>(missing.size());
    if (const auto written = BookInk::write(live, book, page.place.section, {.added = std::move(missing), .removed = {}}, page.anchored);
        !written)
        return std::unexpected(Failure(Failure::Code::InkNotKept, {QString::number(page.place.section + 1)}));
    return count;
}

// One annotation of the earlier book put back in the live one, by its global
// id: come back when it is gone, changed back when it differs. Whether it was.
[[nodiscard]] std::expected<bool, Failure> putAnnotationBack(Library &live, const Library &from, const QList<Annotation> &now, qint64 book,
                                                             const Annotation &earlier) {
    const QString id = Spaces::annotationId(from, earlier.id);
    const qint64 here = Spaces::localId(live, u"annotation"_s, id);
    const auto current = std::ranges::find(now, here, &Annotation::id);
    if (here > 0 && current != now.end() && looksAlike(*current, earlier))
        return false;
    Annotation put = earlier;
    put.id = current == now.end() ? 0 : current->id;
    const auto stored = live.storeAnnotation(book, put);
    if (!stored)
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {earlier.kind}));
    // Come back as the one it was, so a sync takes it for that one again.
    if (put.id == 0)
        Spaces::setGlobalId(live, u"annotation"_s, *stored, id);
    return true;
}

[[nodiscard]] std::expected<void, Failure> putAnnotationsBack(Library &live, const Library &from, qint64 book, qint64 earlier,
                                                              BookRestore::Restored &restored) {
    const QList<Annotation> now = live.annotations(book);
    for (const Annotation &annotation : from.annotations(earlier)) {
        if (annotation.kind == QLatin1StringView(BookInk::kind)) {
            const auto drawn = putStrokesBack(live, from, book, earlier, annotation);
            if (!drawn)
                return std::unexpected(drawn.error());
            restored.strokes += *drawn;
            continue;
        }
        const auto put = putAnnotationBack(live, from, now, book, annotation);
        if (!put)
            return std::unexpected(put.error());
        restored.annotations += *put ? 1 : 0;
    }
    return {};
}

} // namespace

std::expected<BookRestore::Restored, Failure> BookRestore::restore(Library &live, const Library &from, const QString &globalId,
                                                                   Parts parts) {
    const qint64 then = Spaces::localId(from, u"book"_s, globalId);
    const std::optional<Book> earlier = then > 0 ? from.book(then) : std::nullopt;
    if (!earlier)
        return std::unexpected(Failure(Failure::Code::SnapshotBookNotFound, {globalId}));
    const qint64 now = Spaces::localId(live, u"book"_s, globalId);
    const std::optional<Book> current = now > 0 ? live.book(now) : std::nullopt;
    if (!current)
        return std::unexpected(Failure(Failure::Code::RestoreBookGone, {earlier->meta.title}));
    const Library::Batch batch(live);
    if (!batch.begun())
        return std::unexpected(Failure(Failure::Code::LibraryBusy));
    Restored restored;
    if (parts.details) {
        const auto change = LibraryDiff::book(live, from, globalId);
        const auto put = putDetailsBack(live, *current, *earlier, change ? change->details : QList<LibraryDiff::Field>{});
        if (!put)
            return std::unexpected(put.error());
        restored.details = *put;
    }
    if (parts.annotations)
        if (const auto put = putAnnotationsBack(live, from, current->id, earlier->id, restored); !put)
            return std::unexpected(put.error());
    return restored;
}
