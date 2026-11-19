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
// `collectionId`, with `parts`, written by the devices that wrote them in the
// personal space, numbered anew in this one.
[[nodiscard]] std::expected<QList<SpaceOperation>, QString> operations(const Library &library, qint64 collectionId, const Parts &parts,
                                                                       const QString &space);

// The snapshot of the space `owner` publishes the collection as, taken now,
// with the space recorded in the library's spaces as theirs.
[[nodiscard]] std::expected<SpaceSnapshot, QString> snapshot(const Library &library, qint64 collectionId, const Parts &parts,
                                                             const SpaceSnapshot::Owner &owner);

} // namespace PublishedSpace
