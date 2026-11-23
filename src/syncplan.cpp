#include "syncplan.h"

#include <QJsonArray>

#include "spaces.h"

namespace {

// What a removal names a book by: its files' hashes, else its entry keys (SyncTake's takeRemovals).
[[nodiscard]] QJsonArray namesOf(const Library &library, const Book &book) {
    QStringList names;
    for (const FormatFile &file : library.formatFiles(book.id))
        names << file.hash;
    return QJsonArray::fromStringList(names.isEmpty() ? Library::entryKeys(book) : names);
}

void putMeta(SyncPlan::State &state, const BookMeta &meta) {
    state.insert("title", meta.title);
    state.insert("authors", QJsonArray::fromStringList(meta.authors));
    state.insert("series", meta.series);
    state.insert("series_index", meta.seriesIndex);
    state.insert("publisher", meta.publisher);
    state.insert("language", meta.language);
    state.insert("isbn", meta.isbn);
    state.insert("published", meta.published);
    state.insert("description", meta.description);
    state.insert("tags", QJsonArray::fromStringList(meta.tags));
}

void putReading(SyncPlan::State &state, const Book &book) {
    state.insert("shelf", book.shelf);
    state.insert("rating", book.rating);
    state.insert("finished", book.finished);
    state.insert("paper", book.paper);
}

[[nodiscard]] QJsonObject changeJson(const SyncPlan::Change &change) {
    return {{"before", change.before}, {"after", change.after}, {"weak", change.weak}};
}

} // namespace

SyncPlan::State SyncPlan::stateOf(const Library &library, const Book &book) {
    State state{{"gid", Spaces::bookId(library, book.id)},
                {"identifiers", book.identifiers},
                {"cover", !library.coverPath(book.id).isEmpty()},
                {"names", namesOf(library, book)}};
    putMeta(state, book.meta);
    putReading(state, book);
    return state;
}

SyncPlan::States SyncPlan::states(const Library &library) {
    States states;
    for (const Book &book : library.books())
        states.insert(Spaces::bookId(library, book.id), stateOf(library, book));
    return states;
}

QJsonObject SyncPlan::toJson(const States &states) {
    QJsonObject object;
    for (auto it = states.cbegin(); it != states.cend(); ++it)
        object.insert(it.key(), *it);
    return object;
}

SyncPlan::States SyncPlan::statesFrom(const QJsonObject &object) {
    States states;
    for (auto it = object.begin(); it != object.end(); ++it)
        states.insert(it.key(), it.value().toObject());
    return states;
}

SyncPlan::Change &SyncPlan::of(const Library &library, const Book &book) {
    const QString gid = Spaces::bookId(library, book.id);
    auto change = books.find(gid);
    if (change == books.end()) {
        const State state = stateOf(library, book);
        change = books.insert(gid, {.before = state, .after = state, .weak = false});
    }
    return *change;
}

void SyncPlan::details(const Library &library, const Book &book, const BookMeta &meta, bool cover, bool weak) {
    Change &change = of(library, book);
    putMeta(change.after, meta);
    change.after.insert("cover", change.after.value("cover").toBool() || cover);
    change.weak = change.weak || weak;
}

void SyncPlan::reading(const Library &library, const Book &book, const Book &filled) { putReading(of(library, book).after, filled); }

void SyncPlan::identifier(const Library &library, const Book &book, const QString &scheme, const QString &value) {
    Change &change = of(library, book);
    QJsonObject identifiers = change.after.value("identifiers").toObject();
    identifiers.insert(scheme, value);
    change.after.insert("identifiers", identifiers);
}

void SyncPlan::removal(const Library &library, const Book &book) {
    removed.insert(Spaces::bookId(library, book.id), stateOf(library, book));
}

QJsonObject SyncPlan::toJson() const {
    QJsonObject changes;
    for (auto it = books.cbegin(); it != books.cend(); ++it)
        changes.insert(it.key(), changeJson(*it));
    QJsonArray moved;
    for (const Place &place : places)
        moved << QJsonObject{{"title", place.title}, {"before", place.before}, {"after", place.after}};
    return {{"books", changes},
            {"removed", toJson(removed)},
            {"places", moved},
            {"annotations_moved", QJsonArray::fromStringList(annotationsMoved)}};
}

SyncPlan SyncPlan::fromJson(const QJsonObject &object) {
    SyncPlan plan;
    plan.said = !object.isEmpty();
    const QJsonObject changes = object.value("books").toObject();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QJsonObject change = it.value().toObject();
        plan.books.insert(it.key(), {.before = change.value("before").toObject(),
                                     .after = change.value("after").toObject(),
                                     .weak = change.value("weak").toBool()});
    }
    plan.removed = statesFrom(object.value("removed").toObject());
    for (const auto &value : object.value("places").toArray()) {
        const QJsonObject place = value.toObject();
        plan.places << Place{.title = place.value("title").toString(),
                             .before = place.value("before").toInteger(),
                             .after = place.value("after").toInteger()};
    }
    for (const auto &title : object.value("annotations_moved").toArray())
        plan.annotationsMoved << title.toString();
    return plan;
}
