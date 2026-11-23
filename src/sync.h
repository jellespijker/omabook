#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

#include "library.h"
#include "remote.h"
#include "syncplan.h"

// Keeping the libraries of several computers in step.
//
// Each computer has its own whole library and its own database, which only
// its own Omabook ever writes. A sync asks the other computer's Omabook what
// it has (over ssh, which is all Tailscale needs to add), copies across the
// books each side lacks, and carries the newer reading place either way. So
// a laptop reads on the train with no network, nothing is ever opened over
// a network filesystem (where two writers can corrupt a SQLite file), and a
// dropped connection leaves both libraries whole.
//
// A book is the same book on two computers when it has the same global id on
// both, or the change log links the two ids (ADR 0009, #158): once paired, it
// stays paired by that, and nothing else pairs it again. A book never synced
// before is paired by a file with the same content, or, without a file, by an
// identifier of a strong scheme (a valid ISBN, a Goodreads id, a Calibre
// uuid) that names one book on each side, and only when the two titles agree
// (BookPairing); a link an older Omabook made between books whose titles
// disagree is not followed (BookLinks). A book paired on less than its global
// id (by an identifier) never has its title, authors or ISBN changed by a
// sync alone: that is a rename, listed by a dry run, and done only with
// `accept_renames` or `--details` (SyncOptions). A book held twice on one side
// and once on the other is the book of the one copy that is the same by its
// id; should the log know all three as one, a sync decides its details once
// and gives both copies here what it settles on (SyncCopies, #154).
// Of a book both have, the newer details win (Book::detailsAt).
// A book removed on one computer is not fetched back to it, and is removed
// on the other too, to the trash, unless it changed there since. The
// collections and the knowledge graph (graphsync.h) travel too.

// The shape of the manifest two computers exchange. It goes up when a change
// would make an older Omabook misread a newer one's; an older Omabook then
// refuses to sync and says to update, instead of acting on what it misread.
inline constexpr int manifestVersion = 1;

// Who a manifest is for: an Omabook from before the change log, or one that
// speaks it (`omabook manifest --log`). The second's says so ("log": 1) with
// its log's space, device and version vector, names each book by its global
// id, and leaves out what travels by the log: all but what names each book
// and the files (book and cover) to fetch of it.
enum class ManifestFor { OlderPeer, LogPeer };

// Everything a library holds, as the other side of a sync needs to see it.
QJsonObject libraryManifest(const Library &library, ManifestFor peer = ManifestFor::OlderPeer);

struct SyncOptions {
    // Whose details a book both sides have takes where they differ and no
    // time says which are newer (books from before the times were kept):
    // neither (they are only counted), this side's, or the other side's.
    enum class Details { Undecided, Mine, Theirs };
    Details details = Details::Undecided;
    // Whether a book paired on less than its global id takes the title,
    // authors or ISBN the other side gives it (a rename), by their time as
    // the rest of its details. Without it, or `details`, a sync leaves those
    // as they are and lists the rename (#158).
    bool acceptRenames = false;
    // The titles of books a link an older Omabook made folds together with a
    // book of another title, to keep apart from now on (BookLinks::unlink,
    // #160): the way out of a wrong link the titles of its time can't show.
    QStringList unlink;
    // The titles of books whose breaches of the sync guard's rules the person
    // accepts (syncguard.h, #187): a sync with only those goes through.
    QStringList accept;
    // Changes nothing on either side, and says what a sync would do.
    bool dryRun = false;
    // With a hub (#214): pairs this library with the hub's library, when
    // other computers' libraries are paired with that one and this one isn't.
    // Without it, such a sync is refused before anything goes either way.
    bool pair = false;
    // Whether the sync guard planned and checked this sync already, and runs
    // it (SyncGuard::guarded): set by the guard alone.
    bool planned = false;
    // The plugins whose data/ files travel (PluginFileSync); the person's when empty.
    QString plugins;
};

struct SyncSummary {
    int received = 0;           // books that came from the other computer
    int sent = 0;               // books that went to it
    int placesTaken = 0;        // reading places that were newer there
    int placesSent = 0;         // reading places that were newer here
    int detailsTaken = 0;       // books whose details were newer there, taken here
    int detailsSent = 0;        // books whose details were newer here, given there
    int readingTaken = 0;       // books given the shelf, stars or finished day said there
    int readingSent = 0;        // books given there what is said of them here
    int removedHere = 0;        // books removed here, as they were there
    int removedThere = 0;       // books removed there, as they were here
    int detailsUndecided = 0;   // books whose details differ with no time to say which are newer
    int renamedHere = 0;        // books here the other side would rename (#158): renamed with consent, else left
    int renamedThere = 0;       // the same there, as this side would rename them
    int renamesHeld = 0;        // of those, the ones left as they are, for want of consent
    int collectionsTaken = 0;   // collections that changed there last, brought here
    int collectionsSent = 0;    // collections that changed here last, brought there
    int graphTaken = 0;         // things, facts and relations of the knowledge graph new here
    int graphSent = 0;          // the same, new there
    int annotationsTaken = 0;   // bookmarks and highlights made, changed or removed here, as they were there
    int annotationsSent = 0;    // the same there, as they were here
    int refused = 0;            // operations from there refused here (Replication::admit)
    int pluginFilesTaken = 0;   // files of plugins' data/ folders that came from there
    int pluginFilesSent = 0;    // the same, that went there
    int pluginFilesHeld = 0;    // the same, kept where they are, over what a plugin may keep (PluginFiles)
    int pluginFilesOnDemand = 0; // the same, left where they are until a plugin opens them (#135)
    bool dryRun = false;        // nothing was changed; the counts are what would be
    // A sync with a hub refused for the hub's library not being paired with
    // this one (#214): the hub's library, and this one, to offer pairing (#217).
    QString unpairedThere, unpairedHere;
    // The titles each count is of, for showing on a terminal (never logged).
    struct Titles {
        QStringList received, sent, placesTaken, placesSent, detailsTaken, detailsSent, readingTaken, readingSent, removedHere,
            removedThere, detailsUndecided, renamedHere, renamedThere;
        // Books kept apart: they share an identifier but not a title, or an
        // older Omabook linked them as one when they are not (#158).
        QStringList apart;
        // The links `unlink` asked to drop, as "<one title> / <the other>" (#160).
        QStringList unlinked;
        // The identifiers a book had of one kept apart from it, removed on
        // either computer (StrayIdentifiers), as "<title>: <scheme> <value>" (#164).
        QStringList identifiersRemoved;
        // The same that a book kept apart had first, but neither has as its
        // own, so whose it is can't be told: kept, and named (#166).
        QStringList identifiersKept;
    } titles;
    QStringList problems;       // books that could not be carried, and why
    QString error;              // why the sync could not happen at all
    // What the sync guard (syncguard.h, #187) found: each side's plan, as its
    // dry run recorded it; the rules the plans break, each as "<title> (here):
    // <what>", which refuse the sync but for the books accepted; and what the
    // two sides were after the write that the plans didn't announce.
    struct Plans {
        SyncPlan here, there;
    } plans;
    QStringList breaches;
    QStringList unannounced;
    // Whether the other side made no plan, being an Omabook from before the
    // guard: what the sync does there was not checked (#211). A hub that makes
    // none is refused.
    bool uncheckedThere = false;
    // The computers that went by another name at the last sync, on the same
    // machine: known by the name they have now, each said in a line (#215).
    QStringList computersRenamed;

    // The counts as `omabook sync --json` and the MCP server give them, with
    // the titles of a dry run, and of the books whose details are undecided.
    [[nodiscard]] QJsonObject toJson() const;
};

SyncSummary syncLibrary(Library &library, const Remote &remote,
                        const std::function<void(const QString &title)> &progress = {}, const SyncOptions &options = {});
