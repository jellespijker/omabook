#include "olderpeer.h"

#include <compare>

#include "hlc.h"
#include "syncmanifest.h"

void OlderPeer::mergePlace(Library &library, const Remote &remote, qint64 mine, const QJsonObject &theirs, SyncSummary &summary,
                           bool dryRun) {
    const QString title = theirs.value("title").toString();
    const Progress myPlace = library.progress(mine);
    const qint64 theirPlaceHlc = SyncManifest::theirProgressHlc(theirs);
    const qint64 myPlaceHlc = SyncManifest::myProgressHlc(library, mine, myPlace);
    if (std::is_gt(Hlc::compare(theirPlaceHlc, myPlaceHlc))) {
        if (!dryRun) {
            Progress updatedPlace = SyncManifest::progressFrom(theirs.value("progress").toObject());
            updatedPlace.updated = theirPlaceHlc;
            library.setProgress(mine, updatedPlace);
        }
        ++summary.placesTaken;
        summary.titles.placesTaken << title;
    } else if (std::is_gt(Hlc::compare(myPlaceHlc, theirPlaceHlc))) {
        // An older Omabook takes plain seconds for a place's time, never a packed clock.
        Progress sentPlace = myPlace;
        sentPlace.updated = SyncManifest::wireTimestamp(myPlaceHlc, false);
        if (dryRun || remote.omabook(SyncManifest::progressArguments(theirs.value("id").toInteger(), sentPlace)).ok()) {
            ++summary.placesSent;
            summary.titles.placesSent << title;
        }
    }
}

void OlderPeer::givePlace(const Library &library, const Remote &remote, qint64 mine, qint64 theirId) {
    if (Progress place = library.progress(mine); place.updated > 0) {
        place.updated = SyncManifest::wireTimestamp(SyncManifest::myProgressHlc(library, mine, place), false);
        remote.omabook(SyncManifest::progressArguments(theirId, place));
    }
}
