#pragma once

#include <QList>
#include <QString>
#include <QtTypes>
#include <span>

#include "replication.h"

// Compaction of the change log (ADR 0001): a snapshot of each space and the
// operations after it.
//
// Per space, a snapshot (`snapshot_ops`) of the operations that still matter
// up to a mark per device (`snapshot_marks`, what it covers): of each field,
// the latest a device wrote (whatever it wrote before can't win anywhere, in
// any order: SpaceReducer), and every operation of what only grows (where a
// thing was met, a fact, a relation, a reading day, a same-as link). What
// merges or sends the log (Replication, SpaceReplay) reads the snapshot and
// the operations after the mark (`live_ops`), which merge to what all of
// them do, for every device; a peer behind the snapshot is sent the
// snapshot. The snapshot takes the place of the operations it covers (#2):
// they are dropped from `ops` (the one place that may:
// ops-deleted-only-by-compaction), what it keeps of them and what it leaves
// out alike, but only those every member of their space has seen, as it last
// said when syncing (`member_vectors`), so no computer can still need one.
// What reads one thing's operations, or a device's last number, reads the
// snapshot and its marks with `ops`. The snapshot and what came after it is
// `space_ops`, of every space; `live_ops` is the same of the spaces the
// library's rows are made of, without the published ones (#4).
namespace Compaction {

struct Result {
    qint64 operations = 0; // left in `ops`
    qint64 live = 0;       // read in their place: the snapshot and what came after its marks
    qint64 dropped = 0;    // taken out of `ops` by this compaction
};

// Writes the snapshot of every space up to now, and drops what it covers that
// every member has seen. One transaction. The file keeps its size until it is
// vacuumed, unless the library vacuums incrementally.
Result compact(const Library &library);

// Compacts when the operations after the marks outnumber the snapshot and a
// thousand, or when over a thousand are of a device with no mark here, as
// another computer's whole log is when a pairing brings it in (#103): what a
// sync runs after it, so reading the log stays about the size of what it
// says.
void compactWhenWorthIt(const Library &library);

// What a snapshot of one owner's operations keeps of `ops` (a published
// space's, #2): of each field of each thing the latest, all of those that
// share its stamp, and every operation of what only grows
// (SpaceReducer::grows), each value once. Across devices, where compact()
// keeps each device's own: such a space is read only for what it says, never
// for what one computer of it holds. It merges as `ops` does.
[[nodiscard]] QList<SpaceOperation> latest(std::span<const SpaceOperation> ops);

// One operation of the baseline (ADR 0001, Migration 2), by the ids of
// `log_ids`: what the library already was when the log began.
struct BaselineOperation {
    qint64 space = 0;
    qint64 device = 0;
    qint64 seq = 0;
    qint64 hlc = 0;
    QString entity;
    qint64 id = 0;
    QString field;
    QString value;
};

// Writes `op` straight into the snapshot rather than into `ops` (#54): the
// baseline is a snapshot from the start, the latest of each field and every
// operation of what only grows, as compact() would make of it, and the log
// starts after it. It goes unsigned, its author this computer, as the
// operations from before signing do: a baseline only says what the rows
// already were, and a peer takes an unsigned operation only from one of
// one's own computers (Replication::admit). markHeld() then moves each
// device's mark on to the last it holds, so what comes after is numbered on
// from there and sent on top of it.
void holdInSnapshot(const Library &library, const BaselineOperation &op);
void markHeld(const Library &library);

// What `member` held of the log when it synced: per space and device, the
// last sequence number. `member` is named as the spaces name their members
// (space_members): a computer's device id for its personal spaces, a
// person's id for a published or shared one.
void rememberSeen(const Library &library, const QString &member, const Replication::Vector &held);

// Drops a space this computer subscribes to (SubscribedSpace: it is a reader
// of it) from the log whole, its snapshot and what it saw of it too, when it
// unsubscribes: the layer is gone, and nobody needs it from here, since a
// subscription never travels on. A space it writes or shares is never
// dropped this way. How many operations it took out of `ops`.
qint64 forgetSubscribed(const Library &library, const QString &space);

} // namespace Compaction
