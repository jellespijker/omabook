#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "bookmeta.h"

// Reads a Calibre library: the folder Calibre keeps, with its metadata.db.
// Calibre's own record of each book (the title someone corrected, the series,
// the tags, the cover) is worth more than what the files say about
// themselves, so an import takes it from there, along with what Calibre
// knows of the reading: how far each book was read, the stars it was given,
// what other sites call it, and the columns people add to keep track. The
// library is only read.
//
// This is the reader alone. Whether Calibre is used at all is the Calibre
// plugin's to say (calibreplugin.h), which is what the rest of the app asks.
namespace Calibre {

// A highlight, a note on one, or a bookmark made in Calibre's viewer.
struct Note {
    QString id;           // Calibre's own name for it, with its kind, so it comes across once
    QString kind;         // "bookmark", "highlight" or "margin-note"
    int section = 0;      // the chapter file it is in, counted from 0
    QString text;         // the words marked (its passage's), or the bookmark's title
    QString note;         // a margin note's words
    qint64 created = 0;   // seconds since the epoch
    QString color;        // a highlight's colour by Omabook's name for it (HighlightColors), '' for the first
};

// What Calibre knows of how a book was read.
struct Reading {
    QString shelf;        // "to-read", "reading", "read", or empty when nothing says
    int rating = 0;       // 1 to 5 stars, 0 for none
    QString finished;     // the day it was finished, as an ISO date
    // The place it was last read to in Calibre's viewer, on any device.
    double percent = 0;   // 0 to 100
    int section = 0;
    qint64 readAt = 0;    // seconds since the epoch; 0 when it was never opened
    QList<Note> notes;
};

struct Book {
    BookMeta meta;        // without the cover picture
    QStringList files;    // every format Calibre holds, as absolute paths
    QString cover;        // the cover file, or empty
    qint64 id = 0;        // Calibre's number for the book
    QString uuid;         // Calibre's lasting name for it
    // What other sites call it: {"goodreads": "4671", "isbn": "978..."}.
    QMap<QString, QString> identifiers;
    Reading reading;
};

// Whether the folder is a Calibre library.
bool isLibrary(const QString &folder);
// How many books the library holds, or -1 when it can't be read.
int bookCount(const QString &folder);

struct Books {
    QList<Book> books;
    QString error;        // English; empty when the library was read
};
Books read(const QString &folder);

// What a column someone added to Calibre is for, going by its label, its
// name and its type: "shelf", "read", "finished", "progress", "rating", or
// empty for a column that says nothing about reading. The Goodreads Sync
// plugin's columns are among the ones known.
QString columnRole(const QString &label, const QString &name, const QString &datatype);
// The shelf a column's value names ("Currently Reading" is "reading"), or empty.
QString shelfFor(const QString &value);
// The colour of a highlight of Calibre's viewer, by its style
// ({"kind": "color", "type": "builtin", "which": "yellow"}), as Omabook names
// its colours; '' (the first) for a colour Omabook has none like.
QString colorOf(const QJsonObject &style);
// Calibre counts stars in halves, 0 to 10.
int starsFor(int calibreRating);

} // namespace Calibre
