#pragma once

#include <QList>
#include <span>

#include "library.h"
#include "spacereplay.h"

// What the log keeps of a kind of operation this Omabook doesn't know yet
// (#118, OpKinds): a newer Omabook's, come in a sync. Each is stored in
// `ops` as it came, signed as it was, and marked as kept (`kept_ops`): it is
// held and sent on to other computers like any other (Replication), kept
// whole by compaction, but never taken in, so nothing here is made of half
// of what it says. Once an Omabook that knows its kind opens the library,
// after the copy an upgrade keeps first, it is taken in as a sync takes in
// what comes from the computer that wrote it, and kept no more.
namespace KeptOps {

// Of `ops` come from another computer (admitted already), stores and marks
// those of a kind not known here (with `dryRun`, only leaves them out), and
// answers the rest, to be taken in.
[[nodiscard]] QList<SpaceOperation> keep(const Library &library, QList<SpaceOperation> ops, bool dryRun);

// The operations kept here, in order of space, device and number.
[[nodiscard]] QList<SpaceOperation> kept(const Library &library);

// What takes operations in once their kind is known: a sync's own taking,
// in the layer above, given once at start-up. Answers true, for a static.
using Taker = void (*)(Library &, std::span<const SpaceOperation>);
bool takeWith(Taker given) noexcept;

// As a library is set up: of the operations kept, those of a kind known now
// are taken in, by the taker given, and kept no more. Without a taker,
// nothing is (they wait for an Omabook that has one).
void takeKnown(Library &library);

} // namespace KeptOps
