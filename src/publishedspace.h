#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>

#include "spacereplay.h"
#include "spacesnapshot.h"

class Library;

// A published space (ADR 0001, *Spaces*; #1, #2): a collection as its owner
// shares it, read-only for everyone else.
//
// It always holds the collection's tree (each collection's name, parent,
// rule, and how its shelf shows it: view, grouping and sort) and each of its
// books' identity (title, authors, series, ISBN and the names it has
// elsewhere: Goodreads, Open Library, Calibre), by the book's global id.
// Never a book's file, nor where it lies, its formats, cover or tags. What
// is the owner's own goes only as they choose, each part its own switch.
//
// Its operations are those of the owner's personal space that say these
// things, written into the published space and compacted: what the books
// and collections are now, with the times they were written, merging as
// the personal space does.
namespace PublishedSpace {

inline constexpr auto kind = "published";

// The parts of a published space beyond the collection and its books.
struct Parts {
    bool graph = false;     // the knowledge graph of its books: things, what is known of them, how they relate
    bool questions = false; // the questions asked of its books
    bool ratings = false;   // the stars given them
    bool reading = false;   // how far each was read, its shelf and finished day, and the days it was read on

    // By name ("graph", "questions", "ratings", "reading"), as a snapshot keeps
    // them; a name not known here is left out.
    [[nodiscard]] QStringList names() const;
    [[nodiscard]] static Parts fromNames(const QStringList &names);

    bool operator==(const Parts &) const = default;
};

// The id of the space `person` publishes a collection as: the same each time,
// so that publishing it again is publishing the same space.
[[nodiscard]] QString idFor(const QString &person, const QString &collectionGlobalId);

// The operations of the published space `space` of the collection
// `collectionId`, with `parts`, numbered anew in this space. Nothing in them
// names what is above the collection at home or any of the owner's
// computers: collections go by ids of this space, and each computer that
// wrote one by its place among them ("device-001"). Of the graph only what
// the owner has met (GraphContext::reachOf): nothing seen, learned or
// related past where they are in a book, nothing of a book not begun.
[[nodiscard]] std::expected<QList<SpaceOperation>, QString> operations(const Library &library, qint64 collectionId, const Parts &parts,
                                                                       const QString &space);

// A published space as its log has it after following the collection.
struct Publication {
    QString space;
    qint64 appended = 0;   // operations its log took now
    qint64 operations = 0; // in its log, all told
};

// Publishes the collection as the space `owner` publishes it as: the space
// recorded in the library's spaces as theirs, with the collection and parts,
// and its log (PublishedLog) brought in step with what it says now.
// Publishing it again, with other parts or after the collection changed,
// appends to the same log.
[[nodiscard]] std::expected<Publication, QString> publish(const Library &library, qint64 collectionId, const Parts &parts,
                                                          const SpaceSnapshot::Owner &owner);

// Brings the log of a space published here in step with its collection as
// it is now, with the parts it was published with: what a transport runs
// before it sends the space.
[[nodiscard]] std::expected<Publication, QString> follow(const Library &library, const QString &space);

// The snapshot of the space `owner` publishes the collection as, taken now:
// published first, then its log compacted (PublishedLog::compacted), each
// operation keeping its number there and with the owner as its author.
[[nodiscard]] std::expected<SpaceSnapshot, QString> snapshot(const Library &library, qint64 collectionId, const Parts &parts,
                                                             const SpaceSnapshot::Owner &owner);

// The snapshot of a space published here, its log brought in step first:
// what a later snapshot of it is, also once its collection is gone at home
// (then it holds only the removal of all it held).
[[nodiscard]] std::expected<SpaceSnapshot, QString> snapshotOf(const Library &library, const QString &space);

} // namespace PublishedSpace
