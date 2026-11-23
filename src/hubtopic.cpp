#include "manual.h"

// The help's page on the hub and the snapshots: a computer that keeps the
// libraries without a window, the computers linked to it, and the snapshots to
// go back to, where else they are kept and what warns of them. Out of
// helptopics.cpp, which has the other pages and no room for one this size; the
// same rules hold ({commands} as keys, `backticks` typed).

// The hub (#179), a computer linked to it (#180, #189) and synced with it (#188); snapshots (#181, #185), their
// destinations (#182), what changed and going back (#183, #209), and the warnings (#184).
Manual::Topic Manual::hubTopic() {
    return {"hub",
            Manual::tr("The hub and snapshots"),
            Manual::tr("A computer that is always on for your books, and snapshots to go back to."),
            {
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
                            "names the hub's owner, and only that person's computers are taken. Unlink, or `omabook hub remove "
                            "<host>`, takes the key off again.")},
                {Manual::tr("Syncing with the hub"),
                 Manual::tr("Name the hub as the other computer in the settings ({global.settings}) and {library.sync} syncs "
                            "with it, as two computers do; `omabook hub sync` does the same from a terminal, and `--dry-run` "
                            "shows first what it would do. The hub takes a moment snapshot before it writes. Computers still "
                            "sync with each other directly too.")},
                {Manual::tr("Snapshots"),
                 Manual::tr("{library.snapshots}, or `omabook snapshot setup`, sets up encrypted snapshots of the library, "
                            "made with restic: the books, their details and notes, the settings and the plugins' data, each "
                            "keeping only what changed. Its recovery key is shown once: write it down, as without it no one "
                            "can open them. A moment snapshot is taken by itself before a sync, an import, a tidy, an upgrade "
                            "or an agent's change to many books, when something changed since the last. On a hub, `omabook "
                            "snapshot setup --hub` sets them up, and its daemon takes them.")},
                {Manual::tr("Every night"),
                 Manual::tr("A hub takes the night's snapshot by itself; on a computer, `omabook snapshot nightly` does, from a "
                            "timer of your own. The older ones are thinned: every one of 14 days, then 7 daily, 4 weekly, 12 "
                            "monthly, and one of every year.")},
                {Manual::tr("A second disk, and one away from home"),
                 Manual::tr("Destinations, in {library.snapshots} or `omabook snapshot destination add <name> <where>`, are "
                            "more places for the snapshots: a folder on another disk, or a place away from home (sftp:, rest:, "
                            "s3:, b2:, rclone:), each with a recovery key of its own. Each night's snapshots are copied there; "
                            "a disk that isn't plugged in is named, and the others are copied all the same. The panel says "
                            "whether three copies, on two kinds of medium, one away from home, hold.")},
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
            }};
}
