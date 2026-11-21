#pragma once

#include <QStringList>

// The commands that find in a book's text: `find`, by its words, and
// `meaning` (#72), by what it means, in a book or the books of a tenant,
// with the index that takes, made by the embedding server the settings name.
int runFindCli(const QStringList &arguments);
int runMeaningCli(const QStringList &arguments);
