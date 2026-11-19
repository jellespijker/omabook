#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>

#include "sharedspace.h"
#include "subscriptions.h"

class Library;

// The spaces a library is in, and the shared ones (#38) made, joined,
// written and synced: written once for the command line and the MCP server
// (and, later, the window).
//
// Founding a space makes its key (SpaceKey), kept in the user's Omabook data
// folder beside their person (never in a library folder), and links a
// collection here to it: the one named, or a new one of the space's name.
// Inviting someone makes them a member, as an operation in the space, and
// writes an invite file sealed to them (SpaceInvites): only they can open
// it, and it hands them the space's keys. Joining opens it, keeps the keys
// and links a collection of theirs. Removing someone changes the key.
// Syncing goes through a folder something else syncs (SpaceFolder): what is
// new there is taken in by the role rules (SharedSpace), with each new key
// handed over on the way (SharedKeys), filed into the linked collection
// (SharedCollection), a writer's own edits of it written into the space, and
// this computer's new operations written there as sealed batches.
namespace SharedSpaces {

struct Member {
    QString person; // a person's id; a device's for one's own computers
    QString name;   // as it goes by, where known
    QString role;   // "writer" or "reader"
};

struct Summary {
    QString space;
    QString kind; // "personal", "published", "shared" or "world"
    QString name;
    QString role; // this library's (its person's): "writer", "reader", or "" for none yet
    QList<Member> members;
    QString collection; // the collection here linked to it, when there is one
};

// Every space this library is in.
[[nodiscard]] QList<Summary> list(const Library &library);

// Where the user keeps the key `space` was founded with: beside their
// person's key (SharedKeys holds every key of it).
[[nodiscard]] QString keyFile(const Library &library, const QString &space);

// A shared space by its id or its name.
[[nodiscard]] std::expected<QString, QString> resolve(const Library &library, const QString &which);

struct Created {
    Summary space;
    bool newCollection = false;
    qsizetype operations = 0; // of the collection's, written into the space
};
// Founds a shared space called `name` with `parts` (PublishedSpace::Parts by
// name), linked to the collection `collection` here (the space's name when
// empty; made when there is none).
[[nodiscard]] std::expected<Created, QString> create(Library &library, const QString &name, const QStringList &parts,
                                                     const QString &collection = {});

struct Invited {
    Summary space;
    QString person;
    QString role;
    QString file; // the invite written
};
// Makes `person` a member with `role` and writes their invite to `out` (a
// file, or a folder to write it in; the current folder when empty).
[[nodiscard]] std::expected<Invited, QString> invite(Library &library, const QString &which, const QString &person, SharedSpace::Role role,
                                                     const QString &out = {});

// Removes `person` from the space: what they write after this is refused,
// and the space's key changes (SharedKeys::rotate), so that they can't open
// what is written after it, nor have anything taken that they write past
// what this library holds of the space now, whatever its date.
[[nodiscard]] std::expected<Summary, QString> remove(const Library &library, const QString &which, const QString &person);

// Joins the space of the invite in `file`, its books going in the collection
// `as` here (the space's name when empty).
[[nodiscard]] std::expected<Summary, QString> join(Library &library, const QString &file, const QString &as = {});

struct Synced {
    Summary space;
    qsizetype batches = 0; // read from the folder
    qsizetype taken = 0;   // operations taken in
    qsizetype known = 0;   // held already
    qsizetype written = 0; // of this writer's own edits, written into the space
    qsizetype sent = 0;    // operations written to the folder
    QStringList refused;   // each refused batch or operation, and why
    Subscriptions::Report filed;
};
[[nodiscard]] std::expected<Synced, QString> sync(Library &library, const QString &which, const QString &folder);

} // namespace SharedSpaces
