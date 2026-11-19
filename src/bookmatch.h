#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTypes>

#include "bookmeta.h"

class Library;

// Finding the library's own copy of a book known only by what someone else
// says of it (a subscribed space's book, #4), by the rules the rest of
// Omabook already finds a book by, strongest first:
//
// 1. the same file: a book with a file has its smallest file's hash in its
//    global id, so the same id here is the same file;
// 2. a name it has elsewhere (Goodreads, Open Library, Calibre) or its ISBN,
//    as a sync finds a book that has no file (Library::entryKeys);
// 3. its title and authors, as the tidy finds a duplicate
//    (Curation::duplicates), of its sure matches only: one found only once a
//    subtitle is set aside is no match.
namespace BookMatch {

struct Known {
    QString gid; // its global id where it is known
    BookMeta meta;
    QJsonObject identifiers;
};

enum class How { File, Identifier, TitleAndAuthors, None };

struct Found {
    qint64 book = 0; // the library's own, 0 when none
    How how = How::None;
};

// "the same file", "an identifier", "title and authors", or "" for none.
[[nodiscard]] QString describe(How how);

// Of each known book, in their order, the library's own copy.
[[nodiscard]] QList<Found> find(const Library &library, const QList<Known> &known);

} // namespace BookMatch
