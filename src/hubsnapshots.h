#pragma once

#include "hubkeeper.h"
#include "hubschedule.h"
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

[[nodiscard]] SnapshotReason reasonOf(HubKeeper::Moment moment);

} // namespace HubSnapshots
