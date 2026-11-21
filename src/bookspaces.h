#pragma once

#include <QList>
#include <QString>
#include <QtTypes>

class Library;

// The spaces a book goes into beyond the person's own computers (#137): a
// shared space (SharedSpace) or a published one (PublishedSpace), each by
// the collection here that stands for it, which holds the book, by hand, by
// its rule or through a collection inside it. A book a plugin keeps private
// (PrivateBooks) and the person didn't let go is in none of them, whatever
// collections hold it. A plugin asks so that what it writes beside such a
// book is written as for others to read: a note that never points past where
// the reader has read to.
namespace BookSpaces {

struct Held {
    QString space;         // the space's id
    QString kind;          // "shared" or "published"
    QString name;          // what the space is called
    qint64 collection = 0; // the collection here that stands for it
};

// Each shared or published space the book is in; none when it stays one's own.
[[nodiscard]] QList<Held> of(const Library &library, qint64 book);

} // namespace BookSpaces
