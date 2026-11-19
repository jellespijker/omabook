#include "synclog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include "log.h"
#include "replication.h"
#include "spacereplay.h"
#include "spaces.h"

namespace {

[[nodiscard]] QStringList titlesOf(const Library &library, const QList<qint64> &books) {
    QStringList titles;
    for (const qint64 id : books)
        if (const std::optional<Book> book = library.book(id))
            titles << book->meta.title;
    return titles;
}

// The books whose place taking `incoming` changes, or with `dryRun` would.
[[nodiscard]] QList<qint64> takePlaces(Library &library, const QList<SpaceOperation> &incoming, bool dryRun) {
    if (!dryRun)
        return SpaceReplay::takePlaces(library, incoming);
    QList<qint64> books;
    for (const Replication::PlaceChange &change : Replication::placeChanges(library, incoming))
        books << change.book;
    return books;
}

} // namespace

QJsonObject SyncLog::introduction(const Library &library) {
    return {{"space", Spaces::personalSpace(library)},
            {"device", Spaces::deviceId(library)},
            {"vector", Replication::toJson(Replication::vector(library))}};
}

void SyncLog::replicate(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary, bool dryRun) {
    if (!dryRun)
        Spaces::pair(library, manifest.value("space").toString(), manifest.value("device").toString());
    QSet<QString> theirGids;
    for (const auto &book : manifest.value("books").toArray())
        theirGids.insert(book.toObject().value("gid").toString());
    const QList<SpaceOperation> lacking = Replication::since(library, Replication::vectorFrom(manifest.value("vector").toObject()));
    // A dry run links nothing: what is unlinked stays so until a sync.
    const QJsonObject request{{"space", Spaces::personalSpace(library)},
                              {"device", Spaces::deviceId(library)},
                              {"vector", Replication::toJson(Replication::vector(library))},
                              {"ops", Replication::toJson(lacking)},
                              {"books", dryRun ? QJsonArray() : Replication::toJson(Replication::unlinked(library, theirGids))},
                              {"dry_run", dryRun}};
    const auto answer = remote.exchange(QJsonDocument(request), {"sync", "--replicate"});
    if (!answer) {
        summary.problems << "The reading places could not be carried between here and " + remote.host() + ": " + answer.error();
        return;
    }
    const QList<SpaceOperation> incoming = Replication::operationsFrom(answer->value("ops").toArray());
    const QList<qint64> taken = takePlaces(library, incoming, dryRun);
    summary.placesTaken += static_cast<int>(taken.size());
    summary.titles.placesTaken << titlesOf(library, taken);
    summary.placesSent += answer->value("changed").toInt();
    for (const auto &title : answer->value("titles").toArray())
        summary.titles.placesSent << title.toString();
    qCInfo(lcSync).noquote() << "the log with" << remote.host() << ":" << lacking.size() << "operations sent," << incoming.size()
                             << "taken";
}

QJsonObject SyncLog::answer(Library &library, const QJsonObject &request) {
    const bool dryRun = request.value("dry_run").toBool();
    if (!dryRun) {
        Spaces::pair(library, request.value("space").toString(), request.value("device").toString());
        Replication::link(library, Replication::booksFrom(request.value("books").toArray()));
    }
    const QList<qint64> changed = takePlaces(library, Replication::operationsFrom(request.value("ops").toArray()), dryRun);
    // After taking theirs, so the same-as operations just recorded go back with the rest.
    const QList<SpaceOperation> lacking = Replication::since(library, Replication::vectorFrom(request.value("vector").toObject()));
    return {{"ops", Replication::toJson(lacking)},
            {"changed", static_cast<int>(changed.size())},
            {"titles", QJsonArray::fromStringList(titlesOf(library, changed))}};
}
