#pragma once

#include <QtTypes>

class Library;

// Compaction of the change log (ADR 0001): a snapshot of each space and the
// operations after it.
//
// The log is append-only (`ops` is never rewritten nor cut: the harness
// refuses an UPDATE or DELETE of it), so compaction does not drop
// operations from it. It writes, per space, a snapshot (`snapshot_ops`) of
// the operations that still matter up to a mark per device
// (`snapshot_marks`, what the snapshot covers), and what reads the log to
// merge or send it (Replication, SpaceReplay) reads the snapshot and the
// operations after the mark (`live_ops`). An operation that matters is one
// no later operation of the same device overrides: the latest of each field
// a device wrote (whatever it wrote of it before can't win anywhere, over
// any order: SpaceReducer), and every one of what only grows (where a thing
// was met, a fact, a relation, a reading day, a same-as link). So merging the
// snapshot and what came after gives what merging all of them gives, on
// every computer, and a peer is sent no operation another one overrides.
namespace Compaction {

struct Result {
    qint64 operations = 0; // in `ops`
    qint64 live = 0;       // read in their place: the snapshot and what came after its marks
};

// Writes the snapshot of every space up to now.
Result compact(const Library &library);

// Compacts when the operations after the marks outnumber the snapshot and a
// thousand: what a sync runs after it, so reading the log stays about the
// size of what it says.
void compactWhenWorthIt(const Library &library);

} // namespace Compaction
