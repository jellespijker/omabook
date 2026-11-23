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
//   manifest --state                    each book as the sync guard compares it (SyncPlan, #187)
//   sync --replicate <file>             the change log both ways (SyncLog::answer)
//   sync --identity <file>              whether the two libraries may sync (SyncIdentity, #209)
//   sync --plugin-files                 what the plugins' files it keeps hold (PluginFileSync, #212)
//   sync --take-plugin-files <file>     the plugins' files the computer sent, taken in
//   add <file> --meta <file> [--cover <file>] --json
//   add --paper|--wish --meta <file> [--cover <file>] --json
//   add <file> --to <id>
//   set <id> [the options of omabook set] (BookSetting)
//   collections take --json <file>
//   graph take --json <file>
//
// A file it names is one the computer sent into the sync's own folders, by
// its name there: `resolve` says where it is, or refuses it.
//
// The hub keeps the plugins' files of each library (#212) in the library's
// own folder (pluginsOf), every plugin's, as none is installed on a hub: so
// its snapshots hold them with the library, and they are written only here.
// They go to, and come from, only a library paired with the hub's, one of
// its own computers, as the plugins' records in the log do: the sync's device.
namespace HubSyncSteps {

using Resolve = std::function<std::expected<QString, Failure>(const QString &named)>;

// What a step is done for: the sync's folders, whether it is a dry run, and
// the device of the computer's library, as its sync began.
struct Visit {
    Resolve resolve;
    bool dryRun = false;
    QString device;
};

// Where a library on the hub keeps its plugins' files: <its folder>/.plugins/<plugin>/data/.
[[nodiscard]] QString pluginsOf(const QString &libraryRoot);

// Whether `omabook <arguments>` writes the library: all but the manifest and a dry run's log.
[[nodiscard]] bool writes(const QStringList &arguments, const Resolve &resolve);

// What `omabook <arguments>` would have printed, as JSON, or why not.
[[nodiscard]] std::expected<QJsonValue, Failure> run(Library &library, const QStringList &arguments, const Visit &visit);

} // namespace HubSyncSteps
