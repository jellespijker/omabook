#pragma once

#include <QString>
#include <QtTypes>
#include <expected>
#include <optional>

#include "failure.h"
#include "snapshots.h"

class Library;

// The moment snapshots (ADR 0014) taken before the changes no command names
// alone (#208): a library brought from an older format up to date, whatever
// step that takes, and an agent's change to many books at once. What a sync,
// an import or a tidy takes is the command line's (snapshotBeforeRisky) and
// the hub keeper's (HubSnapshots::before).
namespace MomentSnapshot {

// One call of an agent's (an MCP tool, a command it runs) that changes more
// books than this is a bulk change, with a moment snapshot first. The one
// place to change it.
inline constexpr qsizetype bulkBooks = 10;
[[nodiscard]] constexpr bool isBulk(qsizetype books) noexcept { return books > bulkBooks; }

// Before `library` is brought up to date: a moment snapshot, when snapshots
// are set up, and refused when it can't be taken, as on a hub. Without them
// it goes on with a warning: the copy of the database in .backups is all
// there is then. The line to say, or why not (LibraryMigrations::BeforeUpgrade).
// A copy of the snapshots' own (a restore's, in their state folder) is none
// of the person's libraries, and is left as it is.
[[nodiscard]] std::expected<QString, Failure> beforeUpgrade(const Snapshots &snapshots, const Library &library);
// The same with this computer's snapshots: what main() sets for every library
// this program opens (LibraryMigrations::beforeUpgrading).
[[nodiscard]] std::expected<QString, Failure> beforeUpgradeHere(const Library &library);

// What came of the moment snapshot before a bulk change.
struct Bulk {
    QString undo;                      // the snapshot that holds the library as it was; empty when none was taken
    std::optional<Failure> unsnapshot; // why none could be taken, when the change goes on without one
};
// Before a bulk change to `library`: a moment snapshot, when snapshots are set
// up. One that can't be taken refuses the change on a hub, as a write there is
// refused; on a computer it is warned of, and the change goes on.
[[nodiscard]] std::expected<Bulk, Failure> beforeBulk(const Snapshots &snapshots, const Library &library);

} // namespace MomentSnapshot
