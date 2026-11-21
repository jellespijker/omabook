#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

#include "library.h"
#include "remote.h"

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
// A book is the same book on two computers when they hold a file with the
// same content, or, for a book without a file, when it has an entry key
// (identifiers, ISBN, else title and authors) of a book there; never by an
// empty value, nor by bookkeeping such as "goodreads:synced", which every
// book to read shares, and a link a rule like that made before is not
// followed (BookLinks). So one copy
// there can be two here: an entry without a file (from Goodreads) and the
// book's file, of one ISBN. The change log then knows the three as one book,
// and a sync decides its details once, from what both copies here say, and
// gives both of them what it settles on; the copy with a file speaks for the
// book, by its cover and its title, on both sides alike (SyncCopies, #154).
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
    // Changes nothing on either side, and says what a sync would do.
    bool dryRun = false;
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
    // The titles each count is of, for showing on a terminal (never logged).
    struct Titles {
        QStringList received, sent, placesTaken, placesSent, detailsTaken, detailsSent, readingTaken, readingSent,
            removedHere, removedThere, detailsUndecided;
    } titles;
    QStringList problems;       // books that could not be carried, and why
    QString error;              // why the sync could not happen at all

    // The counts as `omabook sync --json` and the MCP server give them, with
    // the titles of a dry run, and of the books whose details are undecided.
    [[nodiscard]] QJsonObject toJson() const;
};

SyncSummary syncLibrary(Library &library, const Remote &remote,
                        const std::function<void(const QString &title)> &progress = {}, const SyncOptions &options = {});
