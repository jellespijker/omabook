#pragma once

#include <QStringList>

class Library;
class QCommandLineParser;

// Importing a shared collection, from a file or a link, following its link,
// and dropping it, as `omabook collections import` and `unsubscribe` do them
// (Subscriptions, Following).
namespace CollectionImport {

// The options of `omabook collections import`, on the parser of `omabook
// collections`: its own, and --as and --dry-run, which share takes too.
void addOptions(QCommandLineParser &parser);

// `omabook collections import <file|link> [--as <name>] [--dry-run]`,
// `omabook collections import --follow [<name|space>] [--yes]` and
// `omabook collections unsubscribe <name|space>`; `words` from the subcommand on.
int run(Library &library, const QCommandLineParser &parser, const QStringList &words, bool json);

} // namespace CollectionImport
