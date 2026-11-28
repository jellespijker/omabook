#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "calibre.h"
#include "failure.h"
#include "library.h"

// What an import (importer.h) does for a Calibre library alone: which of a
// book's files can be taken, with why not for the others, and bringing what
// Calibre knows of the reading into the library.
namespace CalibreImport {

// How a book is named where something of it is left out: its title and first author.
[[nodiscard]] QString nameOf(const Calibre::Book &book);

struct Sifted {
    QStringList files;      // those that can be taken
    QList<Failure> leftOut; // a reason for each of the others
};
// Every format Calibre lists for the book: there, readable and one Omabook takes, or named.
[[nodiscard]] Sifted sift(const Calibre::Book &book);

// What Calibre said of each book the last time it was imported, kept beside
// the library so the next import can tell a value Calibre changed from one
// the user changed here. Each book is written down as it is remembered, in a
// journal beside the file, so an import cut off part way is not forgotten
// (#273); save() folds the journal into the file.
class Memory {
public:
    explicit Memory(const QString &libraryRoot);
    [[nodiscard]] QJsonObject of(qint64 id) const;
    void remember(qint64 id, const QJsonObject &said);
    void save();

private:
    QString m_file, m_journal;
    QJsonObject m_books;
    QJsonObject m_said; // what this one remembered, not folded into the file yet
};

// Brings what Calibre says of a book's reading into the library. A value is
// replaced only while it still is what the last import put there (or nothing,
// for a book never imported from Calibre), so a shelf or a star changed in
// Omabook since stays as the user left it. True when something changed.
[[nodiscard]] bool takeReading(Library &library, qint64 id, const Calibre::Book &calibre, Memory &memory);

} // namespace CalibreImport
