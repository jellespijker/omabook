#include "manual.h"

// The help's page on the hub and the snapshots: a computer that keeps the
// libraries without a window, the computers linked to it, and the snapshots to
// go back to, where else they are kept and what warns of them. Out of
// helptopics.cpp, which has the other pages and no room for one this size; the
// same rules hold ({commands} as keys, `backticks` typed).

namespace {

// The hub (#179), a computer linked to it (#180, #189) and synced with it (#188), by itself (#191), its library
// paired (#214, #217), the plugins' files (#212), its background work (#192), its phones and e-readers (#193), several
// hubs (#196) and the hub in a container (#195).
QList<Manual::Section> hubSections() {
    return {
        {Manual::tr("The hub"),
         Manual::tr("A hub is a computer that stays on, a small server or one at home, and keeps your libraries "
                    "without a window, as their one writer. On it, `omabook hub run --libraries <folder>` chooses the "
                    "folder once; then `systemctl --user enable --now omabook-hub` runs it from then on, and `loginctl "
                    "enable-linger` keeps it running with no one logged in. A unit for a user of its own comes with the "
                    "package too. `omabook hub status` says what it keeps.")},
        {Manual::tr("Linking a computer to it"),
         Manual::tr("{library.hubs}, or `omabook hub add <host>`, signs in to the hub once over your own ssh and puts "
                    "a key of this computer's own there, which opens Omabook's hub session and nothing else: no shell, "
                    "no terminal, no forwarded port. No password is ever asked for or sent. The first computer linked "
                    "names the hub's owner, and only that person's computers are taken. Computers are one person once "
                    "they have synced with each other, so sync a new one with a linked one first (`omabook sync "
                    "<computer>`), then run `omabook hub status <host>` on the linked one, and link it. Unlink, or "
                    "`omabook hub remove <host>`, takes the key off again.")},
        {Manual::tr("Syncing with the hub"),
         Manual::tr("Sync Now in {library.hubs} syncs the library with the hub, as two computers do, and Preview shows "
                    "first what it would do, changing nothing; so does {library.sync} with the hub named as the other "
                    "computer in the settings ({global.settings}), and `omabook hub sync` in a terminal. The hub takes a "
                    "moment snapshot before it writes. Computers still sync with each other directly too.")},
        {Manual::tr("Syncing by itself"),
         Manual::tr("Sync by itself, a switch for each hub in {library.hubs}, or `omabook hub sync <hub> --auto on`, "
                    "is off until you turn it on. Then the library syncs with the hub a little after a change, every "
                    "quarter of an hour and once the hub is back in reach, but waits on battery or a metered "
                    "connection. A sync the hub or the guard refuses stops it, in red with what to do, until a sync by "
                    "hand goes through. The panel's switch below the hubs, or `omabook hub sync --background on`, "
                    "keeps it going with the window closed.")},
        {Manual::tr("One library, paired once"),
         Manual::tr("A library on the hub syncs only with the library it is paired with; one new on the hub is paired "
                    "by its first sync. Another is refused before anything is read or written, and Pair in "
                    "{library.hubs}, or `omabook hub sync <hub>:<library> --pair`, pairs the two after asking: only "
                    "when they are one library.")},
        {Manual::tr("The plugins' files"),
         Manual::tr("A sync with the hub carries the plugins' files too, such as Notebooks' recordings and "
                    "Storyteller's stories. The hub keeps every plugin's, installed there or not, and gives them only "
                    "to your own computers. A file with something in it is never emptied by a sync.")},
        {Manual::tr("Work in the background"),
         Manual::tr("The hub builds the knowledge graph of each new book by itself, one at a time, with the quick "
                    "model of its own settings (`config.json` in its state folder); nothing goes to someone else's "
                    "computer unless hosted quick work is allowed there. The graph reaches your computers by the next "
                    "sync, and nothing past your place shows. {library.hubs} and `omabook hub status` say how far it "
                    "got; Cancel Work, or `omabook hub cancel`, stops it, keeping nothing of that book's job.")},
        {Manual::tr("Phones and e-readers from the hub"),
         Manual::tr("Phones… in {library.hubs} has the hub serve one of its libraries to KOReader and other apps "
                    "that read a catalog, always, with no computer awake. Choose where it may be reached (off until you "
                    "do: the hub only, your tailnet, or your home network), its port, and the name and password "
                    "KOReader syncs its progress with; the page then shows the hub's address and the QR codes to scan. "
                    "Where KOReader says you are reaches your computers by the next sync.")},
        {Manual::tr("Several hubs"),
         Manual::tr("A computer can be linked to more than one hub, and syncs with each. The same library on two hubs "
                    "ends the same, what automatic sync brings from one hub goes on to the others and never back, and "
                    "a hub out of reach holds up none of the others. The first hub to take up a book builds its graph; "
                    "the others leave it to that one and get the graph by the sync.")},
        {Manual::tr("The hub in a container"),
         Manual::tr("On a NAS or a Raspberry Pi, the hub runs from the container image "
                    "`ghcr.io/jellespijker/omabook-hub`, for x86_64 and aarch64, with a Compose file to start from "
                    "(`packaging/container/compose.yaml` in Omabook's source). It runs as a user of its own, never "
                    "root, and its ssh runs nothing but the hub's commands.")},
    };
}

// Snapshots (#181, #185), their destinations (#182, #196), what changed and going back (#183, #209), and the
// warnings (#184).
QList<Manual::Section> snapshotSections() {
    return {
        {Manual::tr("Snapshots"),
         Manual::tr("{library.snapshots}, or `omabook snapshot setup`, sets up encrypted snapshots of the library, "
                    "made with restic: the books, their details and notes, the settings and the plugins' data, each "
                    "keeping only what changed. Its recovery key is shown once: write it down, as without it no one "
                    "can open them; with it, `omabook snapshot setup --repository <where> --recovery-key`, or the panel's "
                    "recovery key field, opens snapshots kept there on another computer. A moment snapshot is taken by itself before a "
                    "sync, an import, a tidy, an upgrade "
                    "or an agent's change to many books, when something changed since the last. On a hub, `omabook "
                    "snapshot setup --hub` sets them up, and its daemon takes them.")},
        {Manual::tr("Every night"),
         Manual::tr("A hub takes the night's snapshot by itself; on a computer, `omabook snapshot nightly` does, from a "
                    "timer of your own. The older ones are thinned: every one of 14 days, then 7 daily, 4 weekly, 12 "
                    "monthly, and one of every year.")},
        {Manual::tr("A second disk, and one away from home"),
         Manual::tr("Destinations, in {library.snapshots} or `omabook snapshot destination add <name> <where>`, are "
                    "more places for the snapshots: a folder on another disk, a place away from home (sftp:, rest:, "
                    "s3:, b2:, rclone:), or another of your hubs (hub:<host>), each with a recovery key of its own. Each "
                    "night's snapshots are copied there; a disk that isn't plugged in is named, and the others are copied "
                    "all the same. The panel says whether three copies, on two kinds of medium, one away from home, hold.")},
        {Manual::tr("What changed, and going back"),
         Manual::tr("Pick a snapshot in {library.snapshots} to see what changed since: the books added, removed and "
                    "changed, detail by detail, and the notes, highlights and knowledge graph counted. A book's "
                    "details or annotations go back as the snapshot has them, after showing what that changes and "
                    "asking, with a moment snapshot first that Undo goes back to; what was made since stays. "
                    "`omabook snapshot diff` and `restore` do the same from a terminal, and `--notebook` puts a "
                    "notebook back.")},
        {Manual::tr("The whole library"),
         Manual::tr("A whole library is restored into a new folder, never over the one in use, and can be opened from "
                    "there. It gets a device id of its own, so it syncs as a computer of its own; a sync with a copy "
                    "of a library refuses, and says what to do.")},
        {Manual::tr("When something is wrong"),
         Manual::tr("{library.snapshots} and `omabook snapshot status` warn, first and in red, with what to do: when "
                    "the newest snapshot is over two days old, a nightly snapshot or a check failed, the password is "
                    "missing, a disk isn't there or is nearly full, or the three copies no longer hold. A new warning "
                    "comes as a desktop notification once, and again a week on while it stands. `omabook snapshot "
                    "check` has restic check them; a hub does every week, and reads them back every month.")},
    };
}

} // namespace

Manual::Topic Manual::hubTopic() {
    return {"hub", Manual::tr("The hub and snapshots"),
            Manual::tr("A computer that is always on for your books, and snapshots to go back to."), hubSections() + snapshotSections()};
}
