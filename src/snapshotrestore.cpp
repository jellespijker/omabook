#include "snapshotrestore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <algorithm>
#include <ranges>
#include <utility>

#include "log.h"
#include "spaces.h"

namespace {

using namespace Qt::StringLiterals;

// The folder a snapshot's database was copied into before restic kept it
// (Snapshots::stage): <state>/staging/<the library's tag>, as it was then.
[[nodiscard]] QString stagingOf(const Restic::Snapshot &snapshot) {
    const QString ending = u"/staging/"_s + snapshot.tagged(u"library:"_s);
    const auto found = std::ranges::find_if(snapshot.paths, [&ending](const QString &path) { return path.endsWith(ending); });
    return found == snapshot.paths.end() ? QString() : *found;
}

// The snapshot's database and what it was taken of (snapshot.json), restored
// into `target`: the folder they are in there.
[[nodiscard]] std::expected<QString, Failure> restoreStaging(const Snapshots &snapshots, const Restic::Snapshot &snapshot,
                                                             const QString &target) {
    const QString staging = stagingOf(snapshot);
    if (staging.isEmpty())
        return std::unexpected(Failure(Failure::Code::SnapshotNotRestored, {u"staging/"_s + snapshot.tagged(u"library:"_s)}));
    if (const auto restored = Restic::restore(snapshots.place().repository, snapshot.id, target, {staging}); !restored)
        return std::unexpected(restored.error());
    QString folder = target + staging;
    if (!QFileInfo::exists(folder + u"/omabook.db"_s))
        return std::unexpected(Failure(Failure::Code::SnapshotNotRestored, {folder + u"/omabook.db"_s}));
    return folder;
}

// The library's folder, as snapshot.json says it was when the snapshot was taken.
[[nodiscard]] QString libraryIn(const QString &staging) {
    QFile manifest(staging + u"/snapshot.json"_s);
    if (!manifest.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(manifest.readAll()).object().value("library"_L1).toString();
}

// Whether a whole library may be restored into `folder`: a new or empty one,
// and neither the library's own nor one inside it.
[[nodiscard]] bool freeFor(const QString &folder, const QString &library) {
    if (folder == library || folder.startsWith(library + u'/'))
        return false;
    const QFileInfo info(folder);
    return !info.exists() || (info.isDir() && QDir(folder).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));
}

// The restored folder and database put where the library is to be.
[[nodiscard]] std::expected<void, Failure> putInPlace(const QString &files, const QString &database, const QString &folder) {
    if (QFileInfo::exists(folder) && !QDir().rmdir(folder))
        return std::unexpected(Failure(Failure::Code::RestoreFolderTaken, {folder}));
    const bool placed = QFileInfo(files).isDir() ? QDir().rename(files, folder) : QDir().mkpath(folder);
    if (!placed || !QFile::rename(database, Library::databaseIn(folder)))
        return std::unexpected(Failure(Failure::Code::SnapshotNotRestored, {folder}));
    return {};
}

// A folder of its own beside the snapshots' state, gone with it: not in /tmp,
// where a large library's database may not fit.
[[nodiscard]] std::expected<std::unique_ptr<QTemporaryDir>, Failure> folderAside(const Snapshots &snapshots, const QString &kind) {
    const QString restoring = snapshots.place().state + u"/restoring"_s;
    if (!QDir().mkpath(restoring))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {restoring}));
    auto folder = std::make_unique<QTemporaryDir>(restoring + u'/' + kind + u"-XXXXXX"_s);
    if (!folder->isValid())
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {restoring}));
    return folder;
}

// The snapshot that undoes a restore about to be written in `live`: one taken
// now, or, when the caller took it (a hub's keeper), the last one.
[[nodiscard]] std::expected<QString, Failure> undoPoint(const Snapshots &snapshots, const Library &live, bool snapshotFirst) {
    if (snapshotFirst)
        return snapshots.take(live, SnapshotReason::Restore).transform([](const Snapshots::Taken &taken) { return taken.id; });
    const auto listed = snapshots.list(live);
    return listed && !listed->isEmpty() ? listed->last().id : QString();
}

} // namespace

std::expected<Restic::Snapshot, Failure> SnapshotRestore::find(const Snapshots &snapshots, const Library &library, const QString &id) {
    const auto listed = snapshots.list(library);
    if (!listed)
        return std::unexpected(listed.error());
    QList<Restic::Snapshot> found;
    if (!id.isEmpty())
        std::ranges::copy_if(*listed, std::back_inserter(found),
                             [&id](const Restic::Snapshot &snapshot) { return snapshot.id.startsWith(id); });
    if (found.isEmpty())
        return std::unexpected(Failure(Failure::Code::SnapshotNotFound, {id}));
    if (found.size() > 1)
        return std::unexpected(Failure(Failure::Code::SnapshotIdAmbiguous, {id}));
    return found.first();
}

SnapshotRestore::Copy::Copy(Restic::Snapshot snapshot, std::unique_ptr<QTemporaryDir> folder, std::unique_ptr<Library> library)
    : m_snapshot(std::move(snapshot)), m_folder(std::move(folder)), m_library(std::move(library)) {}

std::expected<SnapshotRestore::Copy, Failure> SnapshotRestore::Copy::open(const Snapshots &snapshots, const Restic::Snapshot &snapshot) {
    auto folder = folderAside(snapshots, u"copy"_s);
    if (!folder)
        return std::unexpected(folder.error());
    const auto staging = restoreStaging(snapshots, snapshot, (*folder)->filePath(u"restic"_s));
    if (!staging)
        return std::unexpected(staging.error());
    const QString root = (*folder)->filePath(u"library"_s);
    if (!QDir().mkpath(root) || !QFile::rename(*staging + u"/omabook.db"_s, Library::databaseIn(root)))
        return std::unexpected(Failure(Failure::Code::SnapshotNotRestored, {root}));
    auto library = std::make_unique<Library>(root);
    if (QString error; !library->open(&error))
        return std::unexpected(Failure(Failure::Code::SnapshotNotRestored, {error}));
    return Copy(snapshot, std::move(*folder), std::move(library));
}

QJsonObject SnapshotRestore::Compared::toJson() const {
    QJsonObject json = diff.toJson();
    json.insert("from"_L1, SnapshotRestore::toJson(from));
    json.insert("to"_L1, to ? SnapshotRestore::toJson(*to) : QJsonObject{{u"now"_s, true}});
    return json;
}

std::expected<SnapshotRestore::Compared, Failure> SnapshotRestore::compare(const Snapshots &snapshots, const Library &live,
                                                                           const QString &from, const QString &to) {
    const auto earlier = find(snapshots, live, from);
    if (!earlier)
        return std::unexpected(earlier.error());
    const auto before = Copy::open(snapshots, *earlier);
    if (!before)
        return std::unexpected(before.error());
    if (to.isEmpty())
        return Compared{.diff = LibraryDiff::between(before->library(), live), .from = *earlier, .to = std::nullopt};
    const auto later = find(snapshots, live, to);
    if (!later)
        return std::unexpected(later.error());
    const auto after = Copy::open(snapshots, *later);
    if (!after)
        return std::unexpected(after.error());
    return Compared{.diff = LibraryDiff::between(before->library(), after->library()), .from = *earlier, .to = *later};
}

std::expected<SyncIdentity::Renewed, Failure> SnapshotRestore::whole(const Snapshots &snapshots, const Restic::Snapshot &snapshot,
                                                                     const QString &folder) {
    const QString target = QDir::cleanPath(QDir(folder).absolutePath());
    // The database and what it was taken of, first, aside: nothing is written where it goes before it may go there.
    const auto aside = folderAside(snapshots, u"whole"_s);
    if (!aside)
        return std::unexpected(aside.error());
    const auto staging = restoreStaging(snapshots, snapshot, (*aside)->path());
    if (!staging)
        return std::unexpected(staging.error());
    const QString library = QDir::cleanPath(libraryIn(*staging));
    if (library.isEmpty() || library == u"."_s)
        return std::unexpected(Failure(Failure::Code::SnapshotNotRestored, {*staging + u"/snapshot.json"_s}));
    if (!freeFor(target, library))
        return std::unexpected(Failure(Failure::Code::RestoreFolderTaken, {target}));
    // The books' files beside where they go, so they are moved there in one step.
    const QString parent = QFileInfo(target).path();
    if (!QDir().mkpath(parent))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {parent}));
    const QTemporaryDir beside(parent + u"/.omabook-restoring-XXXXXX"_s);
    if (!beside.isValid())
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {parent}));
    if (const auto files = Restic::restore(snapshots.place().repository, snapshot.id, beside.path(), {library}); !files)
        return std::unexpected(files.error());
    if (const auto placed = putInPlace(beside.path() + library, *staging + u"/omabook.db"_s, target); !placed)
        return std::unexpected(placed.error());
    Library restored(target);
    if (QString error; !restored.open(&error))
        return std::unexpected(Failure(Failure::Code::SnapshotNotRestored, {error}));
    SyncIdentity::Renewed renewed = SyncIdentity::renew(restored);
    qCInfo(lcLibrary).noquote() << "snapshot" << snapshot.shortId() << "of" << library << "restored into" << target
                                << "with a device id of its own," << renewed.now << "(it was" << renewed.was + u')';
    return renewed;
}

QJsonObject SnapshotRestore::BookRestored::toJson() const {
    return {{u"restored"_s,
             QJsonObject{{u"details"_s, restored.details}, {u"annotations"_s, restored.annotations}, {u"strokes"_s, restored.strokes}}},
            {u"undo"_s, undo}};
}

std::expected<SnapshotRestore::BookRestored, Failure> SnapshotRestore::book(const Snapshots &snapshots, Library &live, const Copy &from,
                                                                            const QString &globalId, BookRestore::Parts parts,
                                                                            bool snapshotFirst) {
    if (!parts.details && !parts.annotations)
        return std::unexpected(Failure(Failure::Code::RestoreNothingAsked));
    const qint64 earlier = Spaces::localId(from.library(), u"book"_s, globalId);
    const std::optional<Book> then = earlier > 0 ? from.library().book(earlier) : std::nullopt;
    if (!then)
        return std::unexpected(Failure(Failure::Code::SnapshotBookNotFound, {globalId}));
    if (const qint64 now = Spaces::localId(live, u"book"_s, globalId); now <= 0 || !live.book(now))
        return std::unexpected(Failure(Failure::Code::RestoreBookGone, {then->meta.title}));
    BookRestored done;
    const auto undo = undoPoint(snapshots, live, snapshotFirst);
    if (!undo)
        return std::unexpected(undo.error());
    done.undo = *undo;
    const auto restored = BookRestore::restore(live, from.library(), globalId, parts);
    if (!restored)
        return std::unexpected(restored.error());
    done.restored = *restored;
    qCInfo(lcLibrary).noquote() << "book" << globalId << "put back from snapshot" << from.snapshot().shortId() << "in" << live.root()
                                << "; undone by" << done.undo.left(8);
    return done;
}

QJsonObject SnapshotRestore::ThingsRestored::toJson() const {
    return {{u"restored"_s, QJsonObject{{u"things"_s, restored.things}, {u"items"_s, restored.items}}}, {u"undo"_s, undo}};
}

std::expected<SnapshotRestore::ThingsRestored, Failure> SnapshotRestore::things(const Snapshots &snapshots, const Library &live,
                                                                                const PluginRestore::Plan &plan, bool snapshotFirst) {
    const auto undo = undoPoint(snapshots, live, snapshotFirst);
    if (!undo)
        return std::unexpected(undo.error());
    const auto restored = PluginRestore::apply(live, plan);
    if (!restored)
        return std::unexpected(restored.error());
    qCInfo(lcLibrary).noquote() << restored->things << "things of a plugin put back in" << live.root() << "; undone by" << undo->left(8);
    return ThingsRestored{.restored = *restored, .undo = *undo};
}

QJsonObject SnapshotRestore::toJson(const Restic::Snapshot &snapshot) {
    return {{u"id"_s, snapshot.id},
            {u"time"_s, snapshot.time.toString(Qt::ISODate)},
            {u"kind"_s, snapshot.tags.contains(u"nightly"_s) ? u"nightly"_s : u"moment"_s},
            {u"reason"_s, snapshot.tagged(u"reason:"_s)}};
}
