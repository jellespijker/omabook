// The sentences of the snapshots and their destinations (failure.h), a table
// of its own for the file-size budget.

#include <array>

#include "failurewords.h"

namespace {

// As in failure.cpp: marks a sentence for the catalogs (bin/i18n).
constexpr auto tr = [](const char *text, const char * = nullptr, bool = false) { return text; };
constexpr bool counted = true;

constexpr auto wordings = std::to_array<FailureWords::Wording>({
    {Failure::Code::SnapshotsNotSetUp,
     tr("Snapshots aren't set up on this computer yet: omabook snapshot setup makes a place for them, and the recovery key to keep.")},
    {Failure::Code::SnapshotsSetUpAlready, tr("Snapshots are set up already, in %1. Their recovery key was shown when they were set up.")},
    {Failure::Code::SnapshotPasswordUnkept, tr("The snapshots' password couldn't be kept: %1")},
    {Failure::Code::SnapshotNotCopied, tr("The library couldn't be copied for its snapshot: %1")},
    {Failure::Code::ResticMissing, tr("restic (%1) isn't installed here, and snapshots are made with it.")},
    {Failure::Code::ResticFailed, tr("restic couldn't make or read the snapshots: %1")},
    {Failure::Code::ResticNoAnswer, tr("%1 didn't answer within %2 seconds: is it on, and reached from here?")},
    {Failure::Code::SnapshotRepositoryExists,
     tr("There are snapshots in %1 already. To open them, give their recovery key: omabook snapshot setup --repository %1 "
        "--recovery-key, or type it in the Snapshots panel. A destination has a recovery key of its own.")},
    {Failure::Code::SnapshotKeyRefused, tr("The snapshots in %1 can't be opened with that recovery key: %2")},
    {Failure::Code::SnapshotPasswordOther,
     tr("Another snapshot password is kept in %1 already, and it is not written over. Move it away first, and keep it: it may be "
        "all that opens other snapshots.")},
    {Failure::Code::SnapshotNotFound, tr("This library has no snapshot %1 (omabook snapshot list lists them).")},
    {Failure::Code::SnapshotIdAmbiguous, tr("More than one snapshot begins with %1: give more of its id.")},
    {Failure::Code::SnapshotNotRestored, tr("The snapshot couldn't be restored: %1")},
    {Failure::Code::SnapshotBookNotFound, tr("The snapshot holds no book %1.")},
    {Failure::Code::SnapshotBookAmbiguous,
     tr("More than one book is called %1: name it by its global id (omabook snapshot diff --json shows them).")},
    {Failure::Code::RestoreBookGone, tr("%1 is no longer in this library, so nothing of it can be put back into it: restore the whole "
                                        "library into a new folder to have it back.")},
    {Failure::Code::RestoreFolderTaken,
     tr("A library is restored only into a new or empty folder, never over the library or inside it: %1 isn't one.")},
    {Failure::Code::RestoreNothingAsked, tr("Say what to put back of the book: its details, its annotations, or both.")},
    {Failure::Code::DestinationAbsent, tr("The destination %1 isn't there: nothing is kept at %2. Is its disk plugged in and mounted?")},
    {Failure::Code::DestinationUnknown, tr("No destination is called %1 (omabook snapshot destination list lists them).")},
    {Failure::Code::DestinationNameTaken, tr("A destination is called %1 already.")},
    {Failure::Code::DestinationNameInvalid, tr("A destination's name is lower-case letters, digits and dashes, not %1.")},
    {Failure::Code::DestinationPasswordInPlace,
     tr("%1 holds a password, which would be kept in the clear: leave it out, and give it with --credential "
        "RESTIC_REST_PASSWORD (or the place's own) instead.")},
    {Failure::Code::DestinationIsTheRepository, tr("The snapshots are kept in %1 already; a destination is another place.")},
    {Failure::Code::DestinationCredentialMissing, tr("No value was given for %1.")},
    {Failure::Code::DestinationsNotCopied, tr("The snapshots couldn't be copied to %1.")},
    {Failure::Code::SnapshotPasswordMissing,
     tr("The snapshots in %1 can't be opened: their password isn't in %2. Put the recovery key back there, readable by you alone "
        "(chmod 600), or no snapshot is taken there.")},
    {Failure::Code::SnapshotDiskAbsent,
     tr("The disk of %1 isn't there (%2), so no snapshot is copied to it. Plug it in or mount it: the next copy brings it up to date.")},
    {Failure::Code::SnapshotsUnreadable,
     tr("The snapshots in %1 can't be read: %2. See that they can be reached, then look again: omabook snapshot status")},
    {Failure::Code::SnapshotFailed, tr("The last snapshot into %1 failed: %2. Mend what it says, then take one: omabook snapshot now")},
    {Failure::Code::NoSnapshotYet, tr("%1 has no snapshot in %2 yet. Take one: omabook snapshot now")},
    {Failure::Code::SnapshotTooOld,
     tr("The newest snapshot of %1 in %2 is %n hours old. See why none was taken since (omabook snapshot status, and the log), "
        "then take one: omabook snapshot now",
        "", counted)},
    {Failure::Code::SnapshotCheckFailed,
     tr("restic's check of the snapshots in %1 failed: %2. Check them again: omabook snapshot check. Should it fail again, they "
        "may be damaged: take a snapshot elsewhere before trusting them.")},
    {Failure::Code::SnapshotCheckOverdue,
     tr("The snapshots in %1 haven't been checked for %n days. Check them: omabook snapshot check", "", counted)},
    {Failure::Code::SnapshotDataCheckFailed,
     tr("Reading back the snapshots in %1 failed: %2. Read them back again: omabook snapshot check --read-data. Should it fail "
        "again, they are damaged: take a snapshot elsewhere before trusting them.")},
    {Failure::Code::SnapshotDataCheckOverdue,
     tr("The snapshots in %1 haven't been read back for %n days. Read them back: omabook snapshot check --read-data", "", counted)},
    {Failure::Code::SnapshotDiskNearlyFull,
     tr("The disk of the snapshots in %1 is nearly full: %2 left. Make room, or move them to a bigger disk, before a snapshot fails.")},
    {Failure::Code::NoSecondDiskCopy,
     tr("The libraries are no longer kept three times: no second disk holds their snapshots as it should. Mend what is said of "
        "it, or add one: omabook snapshot destination add")},
    {Failure::Code::NoCopyAway,
     tr("The libraries are no longer kept away from home: no place away holds their snapshots as it should. Mend what is said "
        "of it, or add one: omabook snapshot destination add")},
});

} // namespace

std::span<const FailureWords::Wording> FailureWords::snapshots() { return wordings; }
