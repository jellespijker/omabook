#pragma once

#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <expected>
#include <memory>
#include <optional>

#include "bookrestore.h"
#include "failure.h"
#include "library.h"
#include "librarydiff.h"
#include "pluginrestore.h"
#include "restic.h"
#include "snapshots.h"
#include "syncidentity.h"

// Going back to a snapshot (#183, ADR 0014): its library opened beside the
// live one, to compare (LibraryDiff) or to take a book back from
// (BookRestore); or the whole library restored into a new folder, never over
// the live one. A restore that writes into the live library is undoable: a
// moment snapshot is taken first, and one that can't be taken refuses it.
// What `omabook snapshot diff|restore` and the hub's requests (HubSnapshots) call.
namespace SnapshotRestore {

// The snapshot of the library's whose id begins with `id` (a short id will
// do): refused when it is no snapshot of that library, or names more than one.
[[nodiscard]] std::expected<Restic::Snapshot, Failure> find(const Snapshots &snapshots, const Library &library, const QString &id);

// A snapshot's library, its database restored into a folder of its own that
// goes when it does: read and compared, never the live library. Brought up to
// this Omabook's format when it is older, which only that copy sees. Its
// books' files are not restored; its details, annotations and graph are all there.
class Copy {
public:
    [[nodiscard]] static std::expected<Copy, Failure> open(const Snapshots &snapshots, const Restic::Snapshot &snapshot);
    [[nodiscard]] const Library &library() const noexcept { return *m_library; }
    [[nodiscard]] const Restic::Snapshot &snapshot() const noexcept { return m_snapshot; }

private:
    Copy(Restic::Snapshot snapshot, std::unique_ptr<QTemporaryDir> folder, std::unique_ptr<Library> library);

    Restic::Snapshot m_snapshot;
    std::unique_ptr<QTemporaryDir> m_folder;
    std::unique_ptr<Library> m_library;
};

// The library diff from the snapshot `from` to the snapshot `to`, or to the
// live library when `to` is empty.
struct Compared {
    LibraryDiff::Diff diff;
    Restic::Snapshot from;
    std::optional<Restic::Snapshot> to; // none: now

    // The diff's JSON (LibraryDiff::Diff::toJson) with "from" and "to" (a
    // snapshot, or {"now": true}).
    [[nodiscard]] QJsonObject toJson() const;
};
[[nodiscard]] std::expected<Compared, Failure> compare(const Snapshots &snapshots, const Library &live, const QString &from,
                                                       const QString &to = {});

// The whole library as the snapshot holds it, its books' files with it,
// restored into `folder`, which must not exist yet or be empty, and must not be
// the library's own or inside it: a library of its own, opened once to check
// it and given a device id of its own (#209), never two libraries writing as
// one computer: its device id then and now. Its settings and plugins' data/
// folders stay in the snapshot.
[[nodiscard]] std::expected<SyncIdentity::Renewed, Failure> whole(const Snapshots &snapshots, const Restic::Snapshot &snapshot,
                                                                  const QString &folder);

// What a book restore did, and the moment snapshot taken just before it, to
// go back to (its id; the last one's when nothing changed since).
struct BookRestored {
    BookRestore::Restored restored;
    QString undo;

    // {"restored": {"details", "annotations", "strokes"}, "undo"}.
    [[nodiscard]] QJsonObject toJson() const;
};

// The book of `globalId` put back as `from` holds it, in `live`, after a
// moment snapshot of `live`. Call it where `live` is written: the command on a
// computer; inside the keeper's lock on a hub, whose own moment snapshot just
// before is then the one (`snapshotFirst` false). Refused before any snapshot
// when nothing is asked for, or either side lacks the book.
[[nodiscard]] std::expected<BookRestored, Failure> book(const Snapshots &snapshots, Library &live, const Copy &from,
                                                        const QString &globalId, BookRestore::Parts parts, bool snapshotFirst = true);

// What a restore of a plugin's things did (PluginRestore), and the moment
// snapshot taken just before it, as for a book.
struct ThingsRestored {
    PluginRestore::Restored restored;
    QString undo;

    // {"restored": {"things", "items"}, "undo"}.
    [[nodiscard]] QJsonObject toJson() const;
};

// A plugin's thing, a notebook or a page, put back in `live` as the plan made
// of a snapshot's copy says (PluginRestore::plan), after a moment snapshot of
// `live`, as book() does (#209).
[[nodiscard]] std::expected<ThingsRestored, Failure> things(const Snapshots &snapshots, const Library &live,
                                                            const PluginRestore::Plan &plan, bool snapshotFirst = true);

// A snapshot as `snapshot list --json` and the hub's answers show it: {"id", "time", "kind", "reason"}.
[[nodiscard]] QJsonObject toJson(const Restic::Snapshot &snapshot);

} // namespace SnapshotRestore
