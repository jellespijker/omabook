#include "synclog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include "log.h"
#include "replication.h"
#include "spacereplay.h"
#include "spaces.h"
#include "synctake.h"

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

// What the other side took of this one's, as its own summary says it took it.
void countThere(SyncSummary &summary, const QJsonObject &there) {
    const QJsonObject titles = there.value("named").toObject();
    const auto add = [&](int &count, QStringList &named, const char *key) {
        count += there.value(QLatin1StringView(key)).toInt();
        for (const auto &title : titles.value(QLatin1StringView(key)).toArray())
            named << title.toString();
    };
    add(summary.detailsSent, summary.titles.detailsSent, "details_taken");
    add(summary.readingSent, summary.titles.readingSent, "reading_taken");
    summary.collectionsSent += there.value("collections_taken").toInt();
    for (const auto &problem : there.value("problems").toArray())
        summary.problems << problem.toString();
}

} // namespace

QJsonObject SyncLog::introduction(const Library &library) {
    return {{"space", Spaces::personalSpace(library)},
            {"device", Spaces::deviceId(library)},
            {"vector", Replication::toJson(Replication::vector(library))}};
}

QJsonArray SyncLog::replicate(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary,
                              const SyncOptions &options) {
    const bool dryRun = options.dryRun;
    QJsonArray books = manifest.value("books").toArray(); // answered as it is when the log can't go
    if (!dryRun)
        Spaces::pair(library, manifest.value("space").toString(), manifest.value("device").toString());
    QSet<QString> theirGids;
    for (const auto &book : books)
        theirGids.insert(book.toObject().value("gid").toString());
    const QList<SpaceOperation> lacking = Replication::since(library, Replication::vectorFrom(manifest.value("vector").toObject()));
    // A dry run links nothing: what is unlinked stays so until a sync.
    const QJsonObject request{{"space", Spaces::personalSpace(library)},
                              {"device", Spaces::deviceId(library)},
                              {"vector", Replication::toJson(Replication::vector(library))},
                              {"ops", Replication::toJson(lacking)},
                              {"books", dryRun ? QJsonArray() : Replication::toJson(Replication::unlinked(library, theirGids))},
                              {"details", SyncTake::otherSide(options.details)},
                              {"dry_run", dryRun}};
    const auto answer = remote.exchange(QJsonDocument(request), {"sync", "--replicate"});
    if (!answer) {
        summary.problems << "The change log could not be carried between here and " + remote.host() + ": " + answer.error();
        return books;
    }
    const QList<SpaceOperation> incoming = Replication::operationsFrom(answer->value("ops").toArray());
    const QList<qint64> taken = takePlaces(library, incoming, dryRun);
    summary.placesTaken += static_cast<int>(taken.size());
    summary.titles.placesTaken << titlesOf(library, taken);
    summary.placesSent += answer->value("changed").toInt();
    for (const auto &title : answer->value("titles").toArray())
        summary.titles.placesSent << title.toString();
    const QJsonArray described = SyncTake::take(
        library, incoming,
        {.device = manifest.value("device").toString(), .details = options.details, .dryRun = dryRun, .remote = &remote, .books = books},
        summary);
    QJsonObject there = answer->value("taken").toObject();
    there.insert("named", answer->value("named"));
    countThere(summary, there);
    qCInfo(lcSync).noquote() << "the log with" << remote.host() << ":" << lacking.size() << "operations sent," << incoming.size()
                             << "taken";
    return described;
}

QJsonObject SyncLog::answer(Library &library, const QJsonObject &request) {
    const bool dryRun = request.value("dry_run").toBool();
    if (!dryRun) {
        Spaces::pair(library, request.value("space").toString(), request.value("device").toString());
        Replication::link(library, Replication::booksFrom(request.value("books").toArray()));
    }
    const QList<SpaceOperation> incoming = Replication::operationsFrom(request.value("ops").toArray());
    const QList<qint64> changed = takePlaces(library, incoming, dryRun);
    SyncSummary taken;
    taken.dryRun = dryRun;
    // The side asked has no books of the other's to describe: it never fetches any.
    SyncTake::take(library, incoming,
                   {.device = request.value("device").toString(),
                    .details = SyncTake::detailsFrom(request.value("details").toString()),
                    .dryRun = dryRun,
                    .remote = nullptr,
                    .books = {}},
                   taken);
    // After taking theirs, so what was just written here goes back with the rest.
    const QList<SpaceOperation> lacking = Replication::since(library, Replication::vectorFrom(request.value("vector").toObject()));
    return {{"ops", Replication::toJson(lacking)},
            {"changed", static_cast<int>(changed.size())},
            {"titles", QJsonArray::fromStringList(titlesOf(library, changed))},
            {"taken", taken.toJson()},
            {"named", QJsonObject{{"details_taken", QJsonArray::fromStringList(taken.titles.detailsTaken)},
                                  {"reading_taken", QJsonArray::fromStringList(taken.titles.readingTaken)}}}};
}
