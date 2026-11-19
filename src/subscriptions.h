#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>

#include "spacesnapshot.h"
#include "subscribedspace.h"

class Library;

// Importing a shared collection is subscribing to its published space (ADR
// 0001; #4), the same for the command line, the MCP server and, later, the
// window.
//
// The space is kept as a layer (SubscribedSpace): read-only, its operations
// under its own id, never merged into the subscriber's rows. What the
// subscriber gets of their own is one collection, theirs to change like any
// other: made under the space's name or one they choose, or an existing one
// of that name merged into (nothing in it is taken out or overwritten), with
// a collection inside it for each of the space's, and in each the books that
// space's collection holds. A book they have is matched (BookMatch); one they
// don't becomes a wishlist entry, a book without a file (never a download),
// unless they removed it before. The collection is linked to the space, so a
// newer snapshot of it lands in the same one, and files only the books it
// newly brings: what the subscriber did with those filed before stays.
//
// The space's graph stays in the layer, never in the subscriber's own, and
// isn't shown yet: it will be once it can follow their own reading place in
// each book (#5). Copying chosen things of it into their own graph is for
// later. Unsubscribing drops the layer and keeps the collection, with the
// books and wishlist entries it holds: those are the subscriber's own.
namespace Subscriptions {

struct Options {
    QString as;          // the collection's name here; the space's, or the one linked to it, when empty
    bool dryRun = false; // say what would happen, and write nothing
};

// A book of the space, and where it is here.
struct Placed {
    QString title;
    QString authors;
    QString how;             // BookMatch::describe, "wishlist", "removed here before" or "not here any more"
    qint64 book = 0;         // here; 0 for a wishlist entry a dry run would make, or one removed before
    QStringList collections; // the paths here it is put in
};

struct Report {
    SubscribedSpace::Subscription space;
    QString collection; // its path here
    bool newCollection = false;
    bool dryRun = false;
    SubscribedSpace::Accepted operations;
    QList<Placed> books;
    // Of the space's graph, kept in the layer.
    qsizetype things = 0, facts = 0, relations = 0;
};

struct Dropped {
    SubscribedSpace::Subscription space;
    QString collection; // the collection kept here, when there is one
    qint64 operations = 0;
};

// Where a snapshot comes from: a file today. A link (a gist or a repository)
// is another transport for the same snapshot (#7).
[[nodiscard]] std::expected<SpaceSnapshot, QString> fetch(const QString &source);

[[nodiscard]] std::expected<Report, QString> subscribe(Library &library, const SpaceSnapshot &snapshot, const Options &options);
// By the space's id, its name, or the collection linked to it.
[[nodiscard]] std::expected<Dropped, QString> unsubscribe(const Library &library, const QString &which);

[[nodiscard]] QJsonObject toJson(const Report &report);
[[nodiscard]] QJsonObject toJson(const Dropped &dropped);
[[nodiscard]] QStringList describe(const Report &report);
[[nodiscard]] QString describe(const Dropped &dropped);

} // namespace Subscriptions
