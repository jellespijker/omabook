#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

#include "library.h"

// Bringing books into a library, the one way the window and the command line
// both do it: files as they are, folders searched for books, and a Calibre
// library with Calibre's own details, covers and every format of each book.
struct ImportSummary {
    struct Item {
        QString file;
        qint64 id = 0;
        bool duplicate = false;
        // The file joined a book that was here without one (see importBooks).
        bool joined = false;
        QString error;
    };
    QList<Item> items;
    int added = 0;
    int duplicates = 0; // already here: one of the book's files is in the library
    int joined = 0;
    QStringList problems;
    qint64 lastId = 0;
    // From Calibre libraries: the folders that were read as one, how many of
    // their books Calibre knows as read and as being read, and how many books
    // already here had their reading brought up to date.
    QStringList calibreLibraries;
    int read = 0;
    int reading = 0;
    int updated = 0;
};

struct ImportOptions {
    // Whether a folder holding Calibre's metadata.db is read as a Calibre
    // library. This is the Calibre plugin's switch (CalibrePlugin::reads);
    // off, such a folder is searched for books like any other.
    bool calibre = true;
};

// `progress` is told how many of how many books are in, from the calling thread.
//
// A book one of whose files is in the library already is that book: it is
// counted as already here, and only the formats it lacks are copied in. A
// book with none of its files here that is the same as a book here without
// a file (on a paper shelf, wished for, known from a reading site) gives
// that book its file instead of becoming a second one: the wishlist entry a
// space made of this very file (it knows the book by the file's hash, #63),
// else the same by an identifier (an ISBN, a Goodreads or Calibre id), else by title and author
// as tidy finds sure duplicates (Curation::duplicates). The book keeps its
// shelf, stars, identifiers and paper copy; what the import knows fills
// only the details it lacks.
//
// A Calibre library can be imported again and again: a book already here is
// found by its file, nothing is added twice, and what Calibre says of its
// reading (shelf, stars, the day it was finished, the place it was read to,
// identifiers, highlights) is brought up to date. What was changed in Omabook
// since the last import is left alone: the importer remembers what Calibre
// said last time in calibre-import.json in the library folder, and only
// replaces a value that still is what it put there.
ImportSummary importBooks(Library &library, const QStringList &paths,
                          const std::function<void(int done, int total)> &progress = {}, const ImportOptions &options = {});
