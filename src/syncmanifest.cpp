#include "sync.h"

#include <QJsonArray>

#include "collections.h"
#include "graphsync.h"

namespace {

QJsonObject toJson(const Progress &progress) {
    return {{"section", progress.section}, {"fraction", progress.fraction}, {"percent", progress.percent}, {"updated", progress.updated}};
}

} // namespace

QJsonObject libraryManifest(const Library &library) {
    QJsonArray books;
    for (const Book &book : library.books()) {
        QJsonObject object = book.toJson();
        QJsonArray files;
        for (const FormatFile &file : library.formatFiles(book.id))
            files << QJsonObject{{"format", file.format}, {"path", file.path}, {"hash", file.hash}, {"size", file.size}};
        object.insert("files", files);
        const QString cover = library.coverPath(book.id);
        object.insert("cover", cover);
        // So a cover is only carried when it is another picture.
        object.insert("cover_hash", cover.isEmpty() ? QString() : Library::hashFile(cover));
        object.insert("added_at", book.added);
        object.insert("progress", toJson(library.progress(book.id)));
        // What is said of its reading, which a book without a file is all of.
        object.insert("shelf", book.shelf);
        object.insert("paper", book.paper);
        books << object;
    }
    // Each removal with its time, so the other side removes what didn't
    // change since; "removed" is the same without times, for an Omabook
    // from before, which only leaves those alone. An Omabook from before
    // collections, or the graph, reads past them.
    QJsonArray removals;
    for (const Library::Removal &removal : library.removals())
        removals << QJsonObject{{"key", removal.key}, {"at", removal.at}};
    return {{"version", manifestVersion},
            {"omabook", QStringLiteral(OMABOOK_VERSION)},
            {"books", books},
            {"removed", QJsonArray::fromStringList(library.removedHashes())},
            {"removals", removals},
            {"collections", Collections::manifest(library)},
            {"graph", GraphSync::bundle(library)}};
}
