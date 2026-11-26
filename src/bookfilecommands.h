#pragma once

#include <QStringList>

class Library;

// The commands of a book's file: `export`, a copy of it out of the library,
// `embed` (#74), the book's details written into its EPUB, the library's
// own copy, or that file put back as it came in, and `add --to`, another
// format of a book. With `replace`, a file of the same format gives way, to
// the trash; without, it is refused.
int runAddFormats(Library &library, qint64 id, const QStringList &files, bool replace, bool json);
int runExportCli(const QStringList &arguments);
int runEmbedCli(const QStringList &arguments);
