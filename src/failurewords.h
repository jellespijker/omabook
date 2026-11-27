#pragma once

#include <array>
#include <span>

#include "failure.h"

// The sentences of Failure's codes, for failure.cpp and its tables: how one is
// marked, those of the hub, which are many enough to have a file of their own,
// and where to find the snapshots'.
namespace FailureWords {

// Marks a sentence for the catalogs (bin/i18n reads what tr() is handed) and
// gives it back as it is; Failure::text() translates it when it is shown. A
// counted one is marked tr("...", "", counted), and its %n is the failure's count.
constexpr auto tr = [](const char *text, const char * = nullptr, bool = false) { return text; };
constexpr bool counted = true;

struct Wording {
    Failure::Code code;
    const char *text;
};

inline constexpr auto hub = std::to_array<Wording>({
    {Failure::Code::HubNotRunning, tr("No hub is running here: nothing answers at %1. Start it with systemctl, or with omabook hub run.")},
    {Failure::Code::HubRunning, tr("A hub is running here already, answering at %1.")},
    {Failure::Code::HubCantListen, tr("The hub can't take requests at %1: %2")},
    {Failure::Code::HubNoLibraries, tr("The hub has no folder for its libraries yet. Choose one: omabook hub run --libraries <folder>")},
    {Failure::Code::HubNoLibraryYet,
     tr("The hub keeps no library yet, so there is nothing to sync with. Make one on the hub with omabook list --library "
        "\"%1/Books\", then restart the hub.")},
    {Failure::Code::HubNoSuchLibrary,
     tr("The hub keeps no library called %1. A new one is made on the hub with omabook list --library <its libraries "
        "folder>/%1, and kept once the hub is restarted.")},
    {Failure::Code::HubLibraryUnnamed, tr("The hub keeps more than one library, so say which one.")},
    {Failure::Code::HubUnknownRequest, tr("The hub takes no request called %1.")},
    {Failure::Code::HubBadRequest, tr("The hub takes each request as one line of JSON: an object that names its request.")},
    {Failure::Code::HubSecretOpen, tr("The secret %1 can be read by others; only the hub's own user may (chmod 600 it).")},
    {Failure::Code::HubSecretUnreadable, tr("The hub can't read the secret %1: %2")},
    {Failure::Code::HubProtocol, tr("The hub speaks protocol %1, and a session starts with its hello.")},
    {Failure::Code::HubOnlySession, tr("This key opens only the hub's session, never %1.")},
    {Failure::Code::HubNotAComputer, tr("This key no longer opens the hub. Link the computer again: omabook hub add")},
    {Failure::Code::HubNotOwner, tr("The hub belongs to someone else: it takes only its owner's computers. If this computer is "
                                    "yours, sync it with one of yours that is linked to the hub (omabook sync <computer>), "
                                    "run omabook hub status <hub> on that one, and link this one again.")},
    {Failure::Code::HubBadSignature, tr("The link isn't signed by the person it names.")},
    {Failure::Code::HubBadKey, tr("That is no ed25519 public key, which is what the hub takes for a computer.")},
    {Failure::Code::HubBadComputerName, tr("%1 can't name a computer: use letters, digits, dots, dashes and underscores.")},
    {Failure::Code::HubNoSuchComputer, tr("No computer called %1 is linked to the hub.")},
    {Failure::Code::HubKeysUnwritable, tr("The hub can't write its computers' keys to %1: %2")},
    {Failure::Code::HubKeyUnmade, tr("This computer's key for its hubs can't be kept in %1: %2")},
    {Failure::Code::HubUnreachable, tr("The hub at %1 can't be reached, or has no Omabook: %2")},
    {Failure::Code::HubNotLinked, tr("This computer isn't linked to the hub at %1. Link it: omabook hub add %1")},
    {Failure::Code::HubSessionEnded, tr("The session with the hub at %1 ended before it answered.")},
    {Failure::Code::HubSyncHubOlder,
     tr("The hub's library %1 is in an older format (%2) than this computer's (%3): update Omabook on the hub, then sync again.")},
    {Failure::Code::HubSyncHubNewer,
     tr("The hub's library %1 is in a newer format (%2) than this computer's (%3): update Omabook on this computer, then sync again.")},
    {Failure::Code::HubSyncUnguarded,
     tr("Omabook on this computer is older than the sync guard, which checks a sync before it writes anything: update Omabook "
        "on this computer, then sync again.")},
    {Failure::Code::HubSyncHubNoPlan,
     tr("Omabook on the hub at %1 is older than the sync guard, and says nothing of what a sync would change there, so nothing "
        "was changed, here or on the hub: update Omabook on the hub, then sync again.")},
    {Failure::Code::HubSyncNotBegun, tr("No sync of this computer's is under way on the hub: begin it again.")},
    {Failure::Code::HubSyncNoComputer, tr("Only a computer linked to the hub syncs with it.")},
    {Failure::Code::HubSyncRefused, tr("In a sync, the hub takes no %1.")},
    {Failure::Code::HubSyncNotGiven, tr("In a sync, the hub gives only its books, their covers and its plugins' files, never %1.")},
    {Failure::Code::RemoteNoFolder, tr("No folder could be made on %1 for what is sent there: %2")},
    {Failure::Code::HubSyncUnwritten, tr("The hub couldn't keep what it was sent: %1. See that its disk has room.")},
    {Failure::Code::HubSyncNoPlace, tr("In a sync, the hub keeps what it is sent only in that sync's own folders, never at %1.")},
    {Failure::Code::HubSyncDryRun, tr("A dry run changes nothing on the hub.")},
    {Failure::Code::HubSyncNotYours,
     tr("In a sync, the hub gives and takes plugins' files only for a library paired with its own: one of your own computers.")},
    {Failure::Code::HubSyncNotPaired,
     tr("The hub's library %1 isn't paired with this computer's library, so nothing was synced, either way: no books, details, change "
        "log or plugins' files. If the two are one library, pair them once: omabook hub sync <hub>:%1 --pair")},
    {Failure::Code::HubWorkCancelled, tr("Stopped before it was done, so nothing of it was kept.")},
    {Failure::Code::HubWorkNoFolder, tr("The hub can't make the folder %1 to work in.")},
    {Failure::Code::HubWorkUnfinished, tr("The graph of %1 wasn't built to its end, so nothing of it was kept.")},
    {Failure::Code::HubDevicesChoice, tr("The hub can't serve phones and e-readers with %1 set to %2.")},
    {Failure::Code::HubWorkChanged, tr("The graph of %1 changed while the hub worked on it, so nothing was kept; it is done again.")},
    {Failure::Code::HubWorkClaimed,
     tr("Another of your hubs took up the graph of %1 meanwhile, so nothing of this hub's was kept: the graph comes from that one.")},
    {Failure::Code::HubStoreRefused, tr("The hub keeps only the files of snapshots it was sent, never %1.")},
    {Failure::Code::HubStoreUnwritable, tr("The hub can't keep the snapshots' file %1: %2")},
});

// Of the snapshots and their destinations (#181 to #185, #292).
[[nodiscard]] std::span<const Wording> snapshots();

} // namespace FailureWords
