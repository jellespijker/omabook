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
        QString error;
    };
    QList<Item> items;
    int added = 0;
    int duplicates = 0;
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
// A Calibre library can be imported again and again: a book already here is
// found by its file, nothing is added twice, and what Calibre says of its
// reading (shelf, stars, the day it was finished, the place it was read to,
// identifiers, highlights) is brought up to date. What was changed in Omabook
// since the last import is left alone: the importer remembers what Calibre
// said last time in calibre-import.json in the library folder, and only
// replaces a value that still is what it put there.
ImportSummary importBooks(Library &library, const QStringList &paths,
                          const std::function<void(int done, int total)> &progress = {}, const ImportOptions &options = {});
