#include "hubsnapshots.h"

#include <QDateTime>
#include <span>
#include <vector>

#include "library.h"
#include "log.h"

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
