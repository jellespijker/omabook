#pragma once

#include <QList>
#include <QString>
#include <QtTypes>
#include <expected>

#include "spacesnapshot.h"

class Library;

// Book files in spaces (ADR 0001, *Book files*; #63): what travels when a
// space's owner lets its books' files go to everyone it reaches, the same for
// the command line, the MCP server and the window.
//
// Books one owns may be shared with anyone the people sharing the space
// allow, so the switch is the space's, off until someone turns it on: in a
// shared space any writer (it travels in the space as an operation, by the
// role rules), in a published space its owner (a part of it, "files", as
// the graph or the ratings are), and then whoever subscribes gets them too.
//
// With it on, the space says of each book which files it has, by format,
// hash and size (PublishedSpace::filesSaid), never where they lie; a member
// sees the book, and its file is fetched only when wanted, as they open or
// download it, and taken only when its content has the hash the space says.
// It is given to the member's book of it only when that book has no file of
// its own: a copy of theirs, matched as subscribing matches (BookMatch), is
// never replaced. How the files go depends on how the space does:
//
// - A shared space's folder (SpaceFolder): beside its batches, each file
//   sealed with the space's key, written there at each sync by whoever has it.
// - A published space's file (`.omabook-space`): the files go beside it, in a
//   folder of their own (besideOf), as they are, never inside the snapshot,
//   which stays small enough to mail. Whoever subscribes from the file
//   fetches from beside it.
// - A gist (GitHubLink) carries no files: it is no place for large binaries.
//   Sending a space with its files on by a gist says so, and a subscription
//   followed by one says where its files can come from instead.
namespace SpaceFiles {

// Whether the space lets its books' files travel: a shared space as its
// writers last said, a space published here as its owner switched it, and
// one subscribed to as its owner's latest snapshot says.
[[nodiscard]] bool on(const Library &library, const QString &space);

struct Switched {
    QString space;
    QString kind; // "shared" or "published"
    QString name;
    bool on = false;
    bool changed = false; // it was not so already
};
// Switches the files of the space `which` (its id or name) on or off: as a
// writer of a shared space, or the owner of a space published here.
[[nodiscard]] std::expected<Switched, QString> set(const Library &library, const QString &which, bool on);
// Who gets the files now, in English (the command line's language).
[[nodiscard]] QString said(const Switched &switched);

// A file a space carries of a book.
struct Carried {
    QString space;
    QString name; // the space's
    QString format;
    QString hash;
    qint64 size = 0;
};
// The files the spaces carry of the book `book` here, readable ones first;
// none for a book that has a file of its own here, which stays as it is.
[[nodiscard]] QList<Carried> of(const Library &library, qint64 book);

// Fetches a file the spaces carry of the book (of), checks it against its
// hash, and gives it to the book. The file it took.
[[nodiscard]] std::expected<Carried, QString> fetch(Library &library, qint64 book);

// A book a plugin made, whose file the plugin gave the space with its world
// (SpaceWorlds, `file`), travels as above whatever the space's switch, and
// only it (#107): the plugin's user chose to give it, and a member's plugin
// needs the book to read or write in its world. It comes as the space does,
// at a sync or as the space's file is imported, not only when it is opened.
// Of the space's books those, the files fetched here. How many it fetched;
// one not there yet comes at a later sync, or when the book is opened.
qsizetype bringWorldFiles(Library &library, const QString &space);

// Of the space's books, the files this library has and the folder of the
// shared space doesn't yet, written there sealed, when the space lets them
// travel; the folder is remembered as where its files are fetched from. How
// many it wrote.
[[nodiscard]] std::expected<qsizetype, QString> send(const Library &library, const QString &space, const QString &folder);

// The folder beside a space's file where its books' files go: for
// "Shelf.omabook-space", "Shelf.omabook-files".
[[nodiscard]] QString besideOf(const QString &snapshotFile);
// Copies the files of the snapshot's books that this library has beside its
// file, when the space lets them travel. How many it copied.
[[nodiscard]] std::expected<qsizetype, QString> writeBeside(const Library &library, const SpaceSnapshot &snapshot,
                                                            const QString &snapshotFile);
// Remembers the file a subscription was taken from, its books' files beside it.
void takenFrom(const Library &library, const QString &space, const QString &snapshotFile);

} // namespace SpaceFiles
