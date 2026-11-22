#pragma once

#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <expected>
#include <functional>

#include "failure.h"

class Library;

// What a computer's sync asks of a hub's library (#188): the few `omabook`
// commands a sync runs on the other side over ssh (sync.h), taken here as
// the arguments it would give them, and done in the hub's daemon on the
// library its keeper hands it, never by a program of their own (ADR 0013).
// Anything else is refused, and so is everything that writes in a dry run.
//
//   manifest --log                      what the library holds (libraryManifest)
//   sync --replicate <file>             the change log both ways (SyncLog::answer)
//   add <file> --meta <file> [--cover <file>] --json
//   add --paper|--wish --meta <file> [--cover <file>] --json
//   add <file> --to <id>
//   set <id> [the options of omabook set] (BookSetting)
//   collections take --json <file>
//   graph take --json <file>
//
// A file it names is one the computer sent into the sync's own folders, by
// its name there: `resolve` says where it is, or refuses it. Plugins' files
// don't travel through a hub yet (`sync --plugin-files` is refused, so a sync
// leaves them be).
namespace HubSyncSteps {

using Resolve = std::function<std::expected<QString, Failure>(const QString &named)>;

// What `omabook <arguments>` would have printed, as JSON, or why not.
[[nodiscard]] std::expected<QJsonValue, Failure> run(Library &library, const QStringList &arguments, const Resolve &resolve, bool dryRun);

} // namespace HubSyncSteps
