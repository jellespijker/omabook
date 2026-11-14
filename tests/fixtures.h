#pragma once

// Small books of every kind, made on the spot, so the tests need no binary
// files in the repository and each one shows exactly what it reads.

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace Fixtures {

using Entries = QList<QPair<QString, QByteArray>>;

bool write(const QString &path, const QByteArray &contents);
QByteArray picture(int width, int height, const QColor &color, const char *format = "PNG");
bool zip(const QString &path, const Entries &entries);
// An EPUB 2 with two chapters, a stylesheet, a cover, and full metadata.
Entries epubEntries(const QString &title = "The Test Book");
bool epub(const QString &path, const QString &title = "The Test Book");
QByteArray fb2();
bool pdf(const QString &path, const QString &title, const QStringList &pages);
QByteArray be32(quint32 value);
// A Kindle file with only what carries the metadata.
QByteArray mobi();
// A Calibre library in `folder`: one book in two formats with a cover and
// details Calibre corrected, and one in a format Omabook doesn't take.
// Calibre also knows the first book's reading: four stars, read to the end
// on one of two devices, and what Goodreads and Google call it.
bool calibreLibrary(const QString &folder);
// The same library with four more books and the columns people add to keep
// track of their reading (a "read" tick, a shelf, the day it was read, their
// own stars, a percentage): one being read with a highlight and a bookmark,
// one to read, one read long ago, and one whose progress is kept by hand.
bool calibreShelf(const QString &folder);
// Runs statements on a fixture library's metadata.db, as Calibre would
// change it between two imports.
bool calibreChange(const QString &folder, const QStringList &statements);
// Calibre's own settings folder: it names `libraries` (the first as the
// current one), and holds three installed plugins (a metadata source, a file
// type plugin that is switched off in Calibre, and a device driver).
bool calibreConfig(const QString &folder, const QStringList &libraries);

} // namespace Fixtures
