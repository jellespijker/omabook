#include "momentsnapshot.h"

#include <QDir>
#include <QFileInfo>

#include "library.h"
#include "log.h"

namespace {

using namespace Qt::StringLiterals;

[[nodiscard]] QString cleaned(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

// A library inside the snapshots' own state folder: a restore's copy aside.
[[nodiscard]] bool theirOwn(const Snapshots &snapshots, const Library &library) {
    const QString state = snapshots.place().state;
    return !state.isEmpty() && cleaned(library.root()).startsWith(cleaned(state) + u'/');
}

[[nodiscard]] QString saidOf(const Snapshots::Taken &taken) {
    return taken.unchanged ? u"The library's last snapshot, %1, holds it as it is before this."_s.arg(taken.id.left(8))
                           : u"A moment snapshot of the library was taken first: %1."_s.arg(taken.id.left(8));
}

} // namespace

std::expected<QString, Failure> MomentSnapshot::beforeUpgrade(const Snapshots &snapshots, const Library &library) {
    if (theirOwn(snapshots, library))
        return QString();
    if (!snapshots.ready()) {
        qCWarning(lcLibrary).noquote() << "no moment snapshot before bringing" << library.root()
                                       << "up to date: snapshots aren't set up here";
        return u"No moment snapshot was taken first, as snapshots aren't set up here (omabook snapshot setup); "
               "a copy of the database is kept in %1."_s.arg(library.backupFolder());
    }
    const auto first = snapshots.take(library, SnapshotReason::Upgrade);
    if (!first) {
        qCWarning(lcLibrary).noquote() << "no moment snapshot before bringing" << library.root()
                                       << "up to date:" << first.error().english();
        return std::unexpected(Failure(Failure::Code::UpgradeNotSnapshot, {first.error().text()}));
    }
    return saidOf(*first);
}

std::expected<QString, Failure> MomentSnapshot::beforeUpgradeHere(Library &library) {
    return beforeUpgrade(Snapshots(SnapshotPlace::here()), library);
}

std::expected<MomentSnapshot::Bulk, Failure> MomentSnapshot::beforeBulk(const Snapshots &snapshots, const Library &library) {
    if (!snapshots.ready())
        return Bulk{};
    const auto first = snapshots.take(library, SnapshotReason::BulkChange);
    if (first)
        return Bulk{.undo = first->id, .unsnapshot = std::nullopt};
    qCWarning(lcLibrary).noquote() << "no moment snapshot before a bulk change of" << library.root() << ":" << first.error().english();
    if (snapshots.place().hub)
        return std::unexpected(first.error());
    return Bulk{.undo = {}, .unsnapshot = first.error()};
}
