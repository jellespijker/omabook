#include "olderpeer.h"

#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <compare>
#include <optional>

#include "changelog.h"
#include "collections.h"
#include "hlc.h"
#include "log.h"
#include "spacereplay.h"
#include "spaces.h"
#include "syncmanifest.h"
#include "synctake.h"

using namespace Qt::StringLiterals;

namespace {

// Counts one book in a summary, with its title for a terminal.
void note(int &count, QStringList &titles, const QString &title) {
    ++count;
    titles << title;
}

QString remoteDir(const OlderPeer::Sync &sync) {
    if (sync.folderThere.isEmpty())
        sync.folderThere = sync.remote.makeFolder();
    return sync.folderThere;
}

QString writeDetails(const OlderPeer::Sync &sync, const QJsonObject &metaJson) {
    const QString file = sync.scratch.filePath(QString("details-%1.json").arg(++sync.counter));
    QFile json(file);
    return json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(metaJson).toJson()) > 0 ? file : QString();
}

// Whether the other side says less of a book's reading than this one does
// now, so it is given this side's; of its names elsewhere only with `names`.
bool saysLess(const QJsonObject &theirs, const Book &mine, bool names) {
    if ((theirs.value("shelf").toString().isEmpty() && !mine.shelf.isEmpty()) || (theirs.value("rating").toInt() == 0 && mine.rating > 0)
        || (theirs.value("finished").toString().isEmpty() && !mine.finished.isEmpty()) || (!theirs.value("paper").toBool() && mine.paper))
        return true;
    if (!names)
        return false;
    const QJsonObject identifiers = theirs.value("identifiers").toObject();
    return std::ranges::any_of(mine.identifiers.keys(), [&](const QString &scheme) {
        return scheme.compare(QStringLiteral("generated"), Qt::CaseInsensitive) != 0 && !identifiers.contains(scheme);
    });
}

void takeMergedDetails(const OlderPeer::Sync &sync, qint64 mine, const QJsonObject &theirs, const SyncManifest::MergedBookDetails &merged,
                       qint64 settledHlc) {
    BookMeta meta = merged.meta;
    const QString title = theirs.value("title").toString();
    const QString cover = theirs.value("cover").toString();
    const QString picture = sync.scratch.filePath(QString("cover-%1.jpg").arg(++sync.counter));
    if (!sync.options.dryRun && merged.takeCover && !theirs.value("cover_hash").toString().isEmpty() && !cover.isEmpty()
        && sync.remote.fetch(cover, picture))
        meta.cover = QImageReader(picture).read();
    QString error;
    if (sync.options.dryRun || sync.library.setMeta(mine, meta, &error, settledHlc))
        note(sync.summary.detailsTaken, sync.summary.titles.detailsTaken, title);
    else
        sync.summary.problems << title + ": " + error;
}

void sendMergedDetails(const OlderPeer::Sync &sync, qint64 mine, const QJsonObject &theirs, const SyncManifest::MergedBookDetails &merged,
                       qint64 settledHlc, std::optional<qint64> &detailsThere) {
    bool given = sync.options.dryRun;
    const qint64 sentAt = SyncManifest::wireTimestamp(settledHlc, false);
    if (!sync.options.dryRun) {
        const QString folder = remoteDir(sync);
        const QString details = writeDetails(sync, merged.meta.toJson());
        const QString myCoverFile = sync.library.coverPath(mine);
        QStringList set{"set",          QString::number(theirs.value("id").toInteger()),
                        "--meta",       folder + "/details.json",
                        "--details-at", QString::number(sentAt)};
        if (merged.sendCover && !myCoverFile.isEmpty() && sync.remote.send(myCoverFile, folder + "/cover.jpg"))
            set << "--cover" << folder + "/cover.jpg";
        given =
            !folder.isEmpty() && !details.isEmpty() && sync.remote.send(details, folder + "/details.json") && sync.remote.omabook(set).ok();
    }
    const QString title = theirs.value("title").toString();
    if (given) {
        note(sync.summary.detailsSent, sync.summary.titles.detailsSent, title);
        detailsThere = sentAt;
    } else {
        sync.summary.problems << title + ": its details could not be given to " + sync.remote.host() + ".";
    }
}

void mergeDetails(const OlderPeer::Sync &sync, const Book &here, const QJsonObject &theirs, std::optional<qint64> &detailsThere) {
    const SyncManifest::MergedBookDetails merged =
        SyncManifest::mergeBookDetails(sync.library, here, theirs, sync.current, sync.options.details);
    // The details keep the time they had: the later of the two books' own
    // details times, never a field's clock in the log, which a local write
    // moves past what was logged before (ChangeLog's writeField).
    const qint64 settledHlc = std::max(Hlc::normalize(here.detailsAt), Hlc::normalize(theirs.value("details_at").toInteger()));
    if (merged.undecided && !merged.takeFields && !merged.sendFields)
        note(sync.summary.detailsUndecided, sync.summary.titles.detailsUndecided, theirs.value("title").toString());
    if (merged.takeFields)
        takeMergedDetails(sync, here.id, theirs, merged, settledHlc);
    if (merged.sendFields)
        sendMergedDetails(sync, here.id, theirs, merged, settledHlc, detailsThere);
}

// Of a book both sides have, takes their place when it is newer, or gives them this side's.
void mergePlace(const OlderPeer::Sync &sync, qint64 mine, const QJsonObject &theirs) {
    const QString title = theirs.value("title").toString();
    const Progress myPlace = sync.library.progress(mine);
    const qint64 theirPlaceHlc = SyncManifest::theirProgressHlc(theirs);
    const qint64 myPlaceHlc = SyncManifest::myProgressHlc(sync.library, mine, myPlace);
    if (std::is_gt(Hlc::compare(theirPlaceHlc, myPlaceHlc))) {
        if (!sync.options.dryRun) {
            Progress updatedPlace = SyncManifest::progressFrom(theirs.value("progress").toObject());
            updatedPlace.updated = theirPlaceHlc;
            sync.library.setProgress(mine, updatedPlace);
        }
        note(sync.summary.placesTaken, sync.summary.titles.placesTaken, title);
    } else if (std::is_gt(Hlc::compare(myPlaceHlc, theirPlaceHlc))) {
        // An older Omabook takes plain seconds for a place's time, never a packed clock.
        Progress sentPlace = myPlace;
        sentPlace.updated = SyncManifest::wireTimestamp(myPlaceHlc, false);
        if (sync.options.dryRun || sync.remote.omabook(SyncManifest::progressArguments(theirs.value("id").toInteger(), sentPlace)).ok())
            note(sync.summary.placesSent, sync.summary.titles.placesSent, title);
    }
}

// --- Collections, whole, the one changed last winning -------------------

[[nodiscard]] qint64 collectionHlc(const Library &library, const Collection &collection) {
    return std::max(Hlc::normalize(collection.updated),
                    SpaceReplay::entityHlc(library, u"collection"_s, Spaces::collectionId(library, collection.id)));
}

[[nodiscard]] qint64 resolveParentId(Library &library, const QHash<QString, qint64> &ids, const QString &parentPath) {
    if (parentPath.isEmpty())
        return 0;
    if (const qint64 existing = ids.value(parentPath.toLower()))
        return existing;
    const auto made = library.addCollection(parentPath);
    return made ? *made : -1;
}

[[nodiscard]] QSet<qint64> booksWithHashes(const Library &library, const QJsonArray &books) {
    QSet<qint64> ids;
    for (const auto &book : books)
        for (const auto &hash : book.toArray())
            if (const qint64 id = library.bookWithHash(hash.toString())) {
                ids.insert(id);
                break;
            }
    return ids;
}

bool takeCollection(Library &library, const QJsonObject &object, QHash<QString, qint64> &stampedPaths) {
    const QString name = object.value("name").toString().simplified();
    const QString parentPath = object.value("parent").toString();
    if (name.isEmpty())
        return false;
    QHash<QString, Collection> mine;
    QHash<QString, qint64> ids;
    for (const Collection &collection : library.collections(true)) {
        mine.insert(collection.path.toLower(), collection);
        if (!collection.removed)
            ids.insert(collection.path.toLower(), collection.id);
    }
    const QString path = parentPath.isEmpty() ? name : parentPath + "/" + name;
    const auto known = mine.constFind(path.toLower());
    const qint64 theirHlc = Hlc::normalize(object.value("hlc").toInteger(object.value("updated").toInteger()));
    Spaces::observeHlc(library, theirHlc);
    const qint64 myHlc = known != mine.cend() ? collectionHlc(library, *known) : 0;
    if ((known != mine.cend() && std::is_gteq(Hlc::compare(myHlc, theirHlc))) || (known == mine.cend() && object.value("removed").toBool()))
        return false;
    const qint64 parentId = resolveParentId(library, ids, parentPath);
    if (parentId < 0)
        return false;
    Collection collection;
    collection.name = name;
    collection.parent = parentId;
    collection.rule = object.value("rule").toString();
    collection.included = booksWithHashes(library, object.value("included").toArray());
    collection.excluded = booksWithHashes(library, object.value("excluded").toArray());
    collection.shelf = object.value("shelf").toObject();
    collection.format = object.value("format").toObject();
    collection.updated = theirHlc;
    collection.removed = object.value("removed").toBool();
    if (!library.storeCollection(collection))
        return false;
    if (theirHlc > 0)
        stampedPaths.insert(path.toLower(), theirHlc);
    return true;
}

} // namespace

void OlderPeer::mergeBook(const Sync &sync, qint64 mine, bool arrived, const QJsonObject &theirs) {
    const QString title = theirs.value("title").toString();
    // The details: merged field by field by each field's HLC, with the cover
    // when it is another picture. Taken or given, they keep the time they
    // had, so the next sync finds both alike and does nothing.
    std::optional<qint64> detailsThere =
        sync.current ? std::optional<qint64>(SyncManifest::wireTimestamp(SyncManifest::theirDetailsHlc(theirs), false)) : std::nullopt;
    if (const std::optional<Book> here = arrived ? std::nullopt : sync.library.book(mine))
        mergeDetails(sync, *here, theirs, detailsThere);
    // A book without a file was found by its entry keys, which may be another's: names
    // elsewhere go only between copies of one file (#164).
    const bool names = arrived || !theirs.value("files").toArray().isEmpty();
    if (SyncTake::fillReading(sync.library, mine, theirs, sync.options.dryRun, {.take = names, .stray = {}}) && !arrived)
        note(sync.summary.readingTaken, sync.summary.titles.readingTaken, title);
    // Older Omabooks don't say "shelf"; they are given nothing they can't keep.
    if (const std::optional<Book> merged = sync.library.book(mine);
        merged && theirs.contains("shelf") && saysLess(theirs, *merged, names)) {
        if (sync.options.dryRun
            || sync.remote.omabook(SyncTake::readingArguments(theirs.value("id").toInteger(), *merged, detailsThere, names)).ok())
            note(sync.summary.readingSent, sync.summary.titles.readingSent, title);
    }
    mergePlace(sync, mine, theirs);
}

void OlderPeer::removeThere(const Sync &sync, const QJsonObject &theirs, const QStringList &hashes, const QStringList &keys) {
    const QString title = theirs.value("title").toString();
    const qint64 theirId = theirs.value("id").toInteger();
    const std::optional<qint64> when =
        SyncManifest::removedWhen(hashes, keys, [&](const QString &key) { return SyncManifest::localRemovalHlc(sync.library, key); });
    if (!sync.current || !when || !std::is_lteq(Hlc::compare(SyncManifest::lastChangeHlc(theirs), *when)))
        return;
    const Remote::Result removed = sync.options.dryRun ? Remote::Result{.code = 0, .output = {}, .error = {}}
                                                       : sync.remote.omabook({"remove", QString::number(theirId)});
    if (removed.ok()) {
        note(sync.summary.removedThere, sync.summary.titles.removedThere, title);
        if (!sync.options.dryRun)
            qCInfo(lcSync).noquote() << "book" << theirId << "removed on" << sync.remote.host() << "as it was here";
    } else {
        sync.summary.problems << title + " could not be removed on " + sync.remote.host() + ": " + removed.error.section('\n', -1);
    }
}

QSet<QString> OlderPeer::removedThere(const QJsonObject &manifest) {
    QSet<QString> removed;
    for (const auto &hash : manifest.value("removed").toArray())
        removed << hash.toString();
    const QHash<QString, qint64> removals = SyncManifest::remoteRemovalHlcs(manifest);
    for (auto it = removals.cbegin(); it != removals.cend(); ++it)
        removed << it.key();
    return removed;
}

void OlderPeer::removeHere(const Sync &sync, const Book &book, const QStringList &hashes, const QStringList &keys,
                           const QJsonObject &manifest) {
    const QHash<QString, qint64> removalsThere = SyncManifest::remoteRemovalHlcs(manifest);
    const std::optional<qint64> when = SyncManifest::removedWhen(hashes, keys, [&](const QString &key) {
        return removalsThere.contains(key) ? std::optional<qint64>(removalsThere.value(key)) : std::nullopt;
    });
    if (!sync.current || !when || !std::is_lteq(Hlc::compare(SyncManifest::lastChangeHlc(sync.library, book), *when)))
        return;
    const auto removed = sync.options.dryRun ? std::expected<void, QString>() : sync.library.remove(book.id);
    if (removed) {
        note(sync.summary.removedHere, sync.summary.titles.removedHere, book.meta.title);
        if (!sync.options.dryRun)
            qCInfo(lcSync).noquote() << "book" << book.id << "removed here, as it was on" << sync.remote.host();
    } else {
        sync.summary.problems << book.meta.title + " could not be removed here: " + removed.error();
    }
}

void OlderPeer::givePlace(const Library &library, const Remote &remote, qint64 mine, qint64 theirId) {
    if (Progress place = library.progress(mine); place.updated > 0) {
        place.updated = SyncManifest::wireTimestamp(SyncManifest::myProgressHlc(library, mine, place), false);
        remote.omabook(SyncManifest::progressArguments(theirId, place));
    }
}

// What an Omabook from before the log is given of the collections, and takes
// of theirs (`omabook collections take`, which it runs here).
QJsonArray Collections::manifest(const Library &library) {
    ChangeLog::reconcile(library);
    QHash<qint64, QJsonArray> hashes;
    for (const Book &book : library.books()) {
        QJsonArray of;
        for (const FormatFile &file : library.formatFiles(book.id))
            of << file.hash;
        hashes.insert(book.id, of);
    }
    const auto named = [&hashes](const QSet<qint64> &ids) {
        QJsonArray books;
        for (const qint64 id : ids)
            if (!hashes.value(id).isEmpty())
                books << hashes.value(id);
        return books;
    };
    QJsonArray array;
    QHash<qint64, QString> paths;
    for (const Collection &collection : library.collections(true))
        paths.insert(collection.id, collection.path);
    for (const Collection &collection : library.collections(true)) {
        const qint64 hlc = std::max(Hlc::normalize(collection.updated),
                                    SpaceReplay::entityHlc(library, u"collection"_s, Spaces::collectionId(library, collection.id)));
        array << QJsonObject{{"name", collection.name},
                             {"parent", paths.value(collection.parent)},
                             {"rule", collection.rule},
                             {"included", named(collection.included)},
                             {"excluded", named(collection.excluded)},
                             {"shelf", collection.shelf},
                             {"format", collection.format},
                             {"updated", collection.updated},
                             {"hlc", hlc},
                             {"removed", collection.removed}};
    }
    return array;
}

int Collections::take(Library &library, const QJsonArray &theirs) {
    ChangeLog::reconcile(library);
    int changed = 0;
    QHash<QString, qint64> stampedPaths;
    const Library::Batch batch(library);
    for (const auto &value : theirs)
        if (takeCollection(library, value.toObject(), stampedPaths))
            ++changed;
    for (const Collection &collection : library.collections(true)) {
        if (const auto it = stampedPaths.constFind(collection.path.toLower()); it != stampedPaths.cend())
            ChangeLog::syncCollectionAt(library, collection.id, it.value());
    }
    return changed;
}
