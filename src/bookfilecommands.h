#pragma once

#include <QStringList>

// The commands of a book's file: `export`, a copy of it out of the library,
// and `embed` (#74), the book's details written into its EPUB, the library's
// own copy, or that file put back as it came in.
int runExportCli(const QStringList &arguments);
int runEmbedCli(const QStringList &arguments);
