#pragma once

#include "hubkeeper.h"
#include "hubrequests.h"
#include "hubschedule.h"
#include "snapshotdestinations.h"
#include "snapshots.h"

// The hub's snapshots (#181, ADR 0013 and 0014), through the seams the hub's
// daemon (#179) leaves for them. `snapshots` must outlive what these return.
namespace HubSnapshots {

// What the keeper runs before each write that changes much, in its lock: a
// moment snapshot of that library, when something changed since its last.
// One that can't be taken refuses the write; with snapshots not set up on
// the hub, the write goes on.
[[nodiscard]] HubKeeper::Before before(const Snapshots &snapshots);

// The nightly job (HubSchedule::daily): every library's database copied at
// one moment, in the keeper's lock, then restic over each copy and its files
// once the lock is given back, so requests wait only for the copies; then
// the thinning.
[[nodiscard]] HubSchedule::Job nightly(const Snapshots &snapshots);

// The requests of going back to a snapshot (#183), added to the hub's:
//   {"request": "snapshots", "library"}: its snapshots, oldest first ({"snapshots": [...]});
//   {"request": "snapshot-diff", "library", "from", "to"?}: the library diff
//     from one snapshot to another, or to now (SnapshotRestore::Compared::toJson);
//   {"request": "snapshot-restore", "library", "snapshot", "book", "parts": ["details", "annotations"]}:
//     one book put back, in the keeper's lock after its moment snapshot ({"restored", "undo"}).
// A whole library is restored into a new folder on a computer, not through the hub.
void answer(HubRequests &requests, const Snapshots &snapshots);

// The job that copies the snapshots to each destination (#182), after the
// nightly one: one that can't be reached or isn't there (a second disk
// unplugged) fails the job, so it is tried again an hour on, and the others
// are copied all the same. It reads the repositories alone, not the libraries.
[[nodiscard]] HubSchedule::Job destinations(const SnapshotDestinations &destinations);

[[nodiscard]] SnapshotReason reasonOf(HubKeeper::Moment moment);

} // namespace HubSnapshots
