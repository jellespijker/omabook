#include "olderpeer.h"

#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <compare>
#include <optional>

#include "hlc.h"
#include "syncmanifest.h"
#include "synctake.h"

namespace {

// Counts one book in a summary, with its title for a terminal.
void note(int &count, QStringList &titles, const QString &title) {
    ++count;
    titles << title;
}

QString remoteDir(const OlderPeer::Sync &sync) {
    if (sync.folderThere.isEmpty())
        sync.folderThere = QString::fromUtf8(sync.remote.run("mktemp -d").output).trimmed();
    return sync.folderThere;
}

QString writeDetails(const OlderPeer::Sync &sync, const QJsonObject &metaJson) {
    const QString file = sync.scratch.filePath(QString("details-%1.json").arg(++sync.counter));
    QFile json(file);
    return json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(metaJson).toJson()) > 0 ? file : QString();
}

// Whether the other side says less of a book's reading than this one does
// now, so it is given this side's.
bool saysLess(const QJsonObject &theirs, const Book &mine) {
    if ((theirs.value("shelf").toString().isEmpty() && !mine.shelf.isEmpty()) || (theirs.value("rating").toInt() == 0 && mine.rating > 0)
        || (theirs.value("finished").toString().isEmpty() && !mine.finished.isEmpty()) || (!theirs.value("paper").toBool() && mine.paper))
        return true;
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
    if (SyncTake::fillReading(sync.library, mine, theirs, sync.options.dryRun) && !arrived)
        note(sync.summary.readingTaken, sync.summary.titles.readingTaken, title);
    // Older Omabooks don't say "shelf"; they are given nothing they can't keep.
    if (const std::optional<Book> merged = sync.library.book(mine); merged && theirs.contains("shelf") && saysLess(theirs, *merged)) {
        if (sync.options.dryRun
            || sync.remote.omabook(SyncTake::readingArguments(theirs.value("id").toInteger(), *merged, detailsThere)).ok())
            note(sync.summary.readingSent, sync.summary.titles.readingSent, title);
    }
    mergePlace(sync, mine, theirs);
}

void OlderPeer::givePlace(const Library &library, const Remote &remote, qint64 mine, qint64 theirId) {
    if (Progress place = library.progress(mine); place.updated > 0) {
        place.updated = SyncManifest::wireTimestamp(SyncManifest::myProgressHlc(library, mine, place), false);
        remote.omabook(SyncManifest::progressArguments(theirId, place));
    }
}
