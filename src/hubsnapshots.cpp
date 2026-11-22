#include "hubsnapshots.h"

#include <QDateTime>
#include <QJsonArray>
#include <QStringList>
#include <span>
#include <vector>

#include "library.h"
#include "log.h"
#include "snapshotrestore.h"

namespace {

using namespace Qt::StringLiterals;

[[nodiscard]] QString libraryAsked(const QJsonObject &asked) { return asked.value("library"_L1).toString(); }

[[nodiscard]] HubRequests::Answer listed(const Snapshots &snapshots) {
    return [&snapshots](const QJsonObject &asked, HubKeeper &keeper) {
        return keeper.write(libraryAsked(asked), HubKeeper::Moment::None,
                            [&snapshots](Library &library) -> std::expected<QJsonObject, Failure> {
                                const auto found = snapshots.list(library);
                                if (!found)
                                    return std::unexpected(found.error());
                                QJsonArray array;
                                for (const Restic::Snapshot &snapshot : *found)
                                    array.append(SnapshotRestore::toJson(snapshot));
                                return QJsonObject{{u"snapshots"_s, array}};
                            });
    };
}

[[nodiscard]] HubRequests::Answer diffed(const Snapshots &snapshots) {
    return [&snapshots](const QJsonObject &asked, HubKeeper &keeper) {
        const QString from = asked.value("from"_L1).toString();
        const QString to = asked.value("to"_L1).toString();
        return keeper.write(libraryAsked(asked), HubKeeper::Moment::None,
                            [&snapshots, &from, &to](Library &library) -> std::expected<QJsonObject, Failure> {
                                return SnapshotRestore::compare(snapshots, library, from, to).transform(&SnapshotRestore::Compared::toJson);
                            });
    };
}

[[nodiscard]] HubRequests::Answer restored(const Snapshots &snapshots) {
    return [&snapshots](const QJsonObject &asked, HubKeeper &keeper) -> std::expected<QJsonObject, Failure> {
        const QJsonArray parts = asked.value("parts"_L1).toArray();
        const BookRestore::Parts asking{.details = parts.contains(u"details"_s), .annotations = parts.contains(u"annotations"_s)};
        if (!asking.details && !asking.annotations)
            return std::unexpected(Failure(Failure::Code::RestoreNothingAsked));
        const QString id = asked.value("snapshot"_L1).toString();
        const QString book = asked.value("book"_L1).toString();
        // In the keeper's lock, after its moment snapshot (before()).
        return keeper.write(libraryAsked(asked), HubKeeper::Moment::Restore,
                            [&snapshots, &id, &book, asking](Library &library) -> std::expected<QJsonObject, Failure> {
                                const auto snapshot = SnapshotRestore::find(snapshots, library, id);
                                if (!snapshot)
                                    return std::unexpected(snapshot.error());
                                const auto copy = SnapshotRestore::Copy::open(snapshots, *snapshot);
                                if (!copy)
                                    return std::unexpected(copy.error());
                                return SnapshotRestore::book(snapshots, library, *copy, book, asking, false)
                                    .transform(&SnapshotRestore::BookRestored::toJson);
                            });
    };
}

} // namespace

void HubSnapshots::answer(HubRequests &requests, const Snapshots &snapshots) {
    requests.add(u"snapshots"_s, listed(snapshots));
    requests.add(u"snapshot-diff"_s, diffed(snapshots));
    requests.add(u"snapshot-restore"_s, restored(snapshots));
}

SnapshotReason HubSnapshots::reasonOf(HubKeeper::Moment moment) {
    switch (moment) {
    case HubKeeper::Moment::Sync:
        return SnapshotReason::Sync;
    case HubKeeper::Moment::Import:
        return SnapshotReason::Import;
    case HubKeeper::Moment::Tidy:
        return SnapshotReason::Tidy;
    case HubKeeper::Moment::Merge:
        return SnapshotReason::Merge;
    case HubKeeper::Moment::Restore:
        return SnapshotReason::Restore;
    case HubKeeper::Moment::Upgrade:
        return SnapshotReason::Upgrade;
    case HubKeeper::Moment::Bulk:
        return SnapshotReason::BulkChange;
    case HubKeeper::Moment::None:
    case HubKeeper::Moment::Asked:
        break;
    }
    return SnapshotReason::Asked;
}

HubKeeper::Before HubSnapshots::before(const Snapshots &snapshots) {
    return [&snapshots](Library &library, HubKeeper::Moment moment) -> std::expected<void, Failure> {
        if (moment == HubKeeper::Moment::None)
            return {};
        const auto taken = snapshots.before(library, reasonOf(moment));
        if (!taken)
            return std::unexpected(taken.error());
        return {};
    };
}

HubSchedule::Job HubSnapshots::nightly(const Snapshots &snapshots) {
    return [&snapshots](HubKeeper &keeper) -> std::expected<void, Failure> {
        if (!snapshots.ready()) {
            qCInfo(lcLibrary) << "No nightly snapshot: snapshots aren't set up on this hub (omabook snapshot setup --hub).";
            return {};
        }
        std::vector<Snapshots::Staged> staged;
        if (const auto copied = keeper.whole([&snapshots, &staged](std::span<Library *const> libraries) -> std::expected<void, Failure> {
                for (const Library *library : libraries) {
                    auto one = snapshots.stage(*library);
                    if (!one)
                        return std::unexpected(one.error());
                    staged.push_back(std::move(*one));
                }
                return {};
            });
            !copied)
            return std::unexpected(copied.error());
        const QDateTime now = QDateTime::currentDateTime();
        for (const Snapshots::Staged &one : staged) {
            if (const auto taken = snapshots.keep(one, SnapshotReason::Nightly); !taken)
                return std::unexpected(taken.error());
            if (const auto thinned = snapshots.thin(one.tag, now); !thinned)
                return std::unexpected(thinned.error());
        }
        return {};
    };
}

HubSchedule::Job HubSnapshots::destinations(const SnapshotDestinations &destinations) {
    return [&destinations](HubKeeper & /*keeper*/) -> std::expected<void, Failure> {
        const SnapshotDestinations::Round round = destinations.copyAll(QDateTime::currentDateTime());
        if (round.failed.isEmpty())
            return {};
        QStringList names;
        for (const auto &[name, failure] : round.failed)
            names << name;
        return std::unexpected(Failure(Failure::Code::DestinationsNotCopied, {names.join(u", ")}));
    };
}
