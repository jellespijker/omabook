#pragma once

#include <QCommandLineParser>
#include <QStringList>

class Library;

// The commands of a book's file: `export`, a copy of it out of the library,
// `embed` (#74), the book's details written into its EPUB, the library's
// own copy, or that file put back as it came in, and `add --to`, another
// format of a book. With `replace`, a file of the same format gives way, to
// the trash; without, it is refused.
int runAddFormats(Library &library, qint64 id, const QStringList &files, bool replace, bool json);
// `add`'s options that describe a book with no file, and the two a sync gives with a book it sends (#255):
// the id the book has on the computer it comes from, and the hash its file came in with there (#74).
void addDescribingOptions(QCommandLineParser &parser);
int runExportCli(const QStringList &arguments);
int runEmbedCli(const QStringList &arguments);
