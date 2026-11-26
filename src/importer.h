#pragma once

#include <QJsonArray>
#include <QJsonObject>
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
        [[nodiscard]] QJsonObject toJson() const;
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
    // What a Calibre library holds that didn't come across, each with why.
    // `skipped` of its books didn't at all; those left out for want of a file
    // are named first, those that failed are among the problems.
    QList<Failure> leftOut;
    int skipped = 0;

    [[nodiscard]] bool foundNothing() const;
    // Something went wrong, or a book stayed behind.
    [[nodiscard]] bool failed() const;
    // The problems and what was left out, as sentences to show.
    [[nodiscard]] QStringList reasons() const;
    [[nodiscard]] QJsonArray itemsJson() const;
    // What importing Calibre libraries did, as JSON and as one line in English.
    [[nodiscard]] QJsonObject calibreJson() const;
    [[nodiscard]] QString calibreLine() const;
    // The log names all of it, however little of it is said at once.
    void keepInLog() const;
};

struct ImportOptions {
    // Whether a folder holding Calibre's metadata.db is read as a Calibre
    // library. This is the Calibre plugin's switch (CalibrePlugin::reads);
    // off, such a folder is searched for books like any other.
    bool calibre = true;
};

// Nothing of a Calibre library is passed over in silence: a book, a format of
// one or its cover that doesn't come across is named, in the summary and the log.
//
// `progress` is told how many of how many books are in, from the calling thread.
//
// A book one of whose files is in the library already is that book: it is
// counted as already here, and only the formats it lacks are copied in, also
// when others are adding it at the same time (addinghold.h). A
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
