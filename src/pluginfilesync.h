#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

#include "library.h"
#include "remote.h"
#include "sync.h"

// The part of a sync that carries plugins' data/ files (#131, PluginFiles):
// before the log goes, each side records what its plugins' folders hold;
// after, when both logs say the same, each file one side lacks goes there
// once, by its hash, and what was removed goes on the side that still has it.
// Only for a plugin installed on the side that takes the file; the others'
// wait in the log until it is. Between two Omabooks that speak the log, and
// never in a dry run.
namespace PluginFileSync {

// This computer's side, `omabook sync --plugin-files`: records what each
// plugin installed in `plugins` (the person's folder when empty) holds, and
// answers it: {"folder", "plugins": {id: {path: {hash, size}}}, "held"}.
[[nodiscard]] QJsonObject survey(const Library &library, const QString &plugins = {});
// `omabook sync --take-plugin-files <request>`: puts each file the request
// names ({"files": [{plugin, path, file}]}) where it goes, as PluginFiles::take
// does, and removes what the log says was removed: {"taken", "removed"}.
[[nodiscard]] QJsonObject take(const Library &library, const QJsonObject &request, const QString &plugins = {});

// What each side holds, as survey() answers it.
struct Sides {
    QJsonObject here, there;
};
// Here, before the log goes: none when there knows nothing of plugins' files.
[[nodiscard]] std::optional<Sides> begin(const Library &library, const Remote &remote, const QString &plugins);
// Here, once it went: fetches and sends what each side lacks, counted in `summary`.
void finish(const Library &library, const Remote &remote, const QString &plugins, const Sides &sides, SyncSummary &summary);

} // namespace PluginFileSync
