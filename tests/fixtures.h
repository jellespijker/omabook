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

// The home every test program runs in: a folder of its own, made before main
// and removed at the end, with HOME and the XDG data, config, cache and state
// folders inside it. Whatever a test opens without saying where (the person's
// key, config.json, the plugins, the caches, the log), and every omabook it
// runs, finds this folder, never the user's own home.
const QString &home();

using Entries = QList<QPair<QString, QByteArray>>;

bool write(const QString &path, const QByteArray &contents);
QByteArray picture(int width, int height, const QColor &color, const char *format = "PNG");
bool zip(const QString &path, const Entries &entries);
// An EPUB 2 with two chapters, a stylesheet, a cover, and full metadata.
Entries epubEntries(const QString &title = "The Test Book");
bool epub(const QString &path, const QString &title = "The Test Book");
QByteArray fb2();
bool pdf(const QString &path, const QString &title, const QStringList &pages);
// A book of chapters, each a title and its body (XHTML inside <body>), with
// the contents listing each; a picture named "images/plate.png" (a tall
// one, 600 by 800) is there for a body to show.
struct Chapter {
    QString title, body;
};
bool chapteredEpub(const QString &path, const QString &title, const QList<Chapter> &chapters);
// A PDF of `pages` portrait pages (A5), each saying its number large and
// with lines of text under it, and an outline of chapters by the page each
// starts on (from 0), so it reads like a book.
bool outlinedPdf(const QString &path, const QString &title, int pages, const QList<QPair<QString, int>> &chapters);
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

// A program for the tests to run: a shell script at `path`.
QString script(const QString &path, const QByteArray &body);
// A stand-in for ssh that runs the command on this computer, with the
// omabook at `binary`, as another computer whose home is "there-home" in
// `folder` (its trash too). Every folder of its own, never this computer's:
// an omabook command reads the settings in XDG_CONFIG_HOME and, on its way
// out, looks at every library they name, which would open the user's own.
QString standInSsh(const QString &folder, const QString &binary);

// Why a result (a std::expected) failed, for a check's message: Qt makes a
// QVERIFY2's message also when the check passes, and a result that holds a
// value has no error to read (calling error() on it aborts under
// _GLIBCXX_ASSERTIONS). Empty when there is a value.
template <typename Result> [[nodiscard]] QString whyNot(const Result &result) { return result ? QString() : QString(result.error()); }

} // namespace Fixtures
