#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <cstdint>
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
// never in a dry run. A file the plugin marked to travel on demand (#135)
// goes neither way: the side that lacks it remembers where it is, and
// fetches it once the plugin asks for it (fetch).
//
// A hub (#212) keeps every plugin's files, installed there or not, for the
// person's computers to take: it is sent them all, those on demand too, and
// gives them whole. Like the plugins' records in the log, the files go only
// to one's own computers: a side whose device isn't one of this library's
// (Spaces::isOwnComputer, once the log went) is given none and asked for none.
// And as the sync guard keeps a book's fields (#187), a sync never empties a
// file that has something in it: the log may say a file holds nothing, but
// the side that holds more keeps it, and says so, until it goes the other way.
namespace PluginFileSync {

// Whose files a side keeps: those of the plugins installed in its folder (a
// computer's), or of every plugin, each in a folder by its id (a hub's).
enum class Keeping : std::uint8_t { Installed, Every };

// This computer's side, `omabook sync --plugin-files`: records what each
// plugin installed in `plugins` (the person's folder when empty) holds, and
// answers it: {"folder", "plugins": {id: {path: {hash, size}}}, "held",
// "device"}, with "every": true when it keeps every plugin's.
[[nodiscard]] QJsonObject survey(const Library &library, const QString &plugins = {}, Keeping keeping = Keeping::Installed);
// `omabook sync --take-plugin-files <request>`: puts each file the request
// names ({"files": [{plugin, path, file}], "device"}) where it goes, as
// PluginFiles::take does, and removes what the log says was removed:
// {"taken", "removed"}. A request from a device not one of this library's own
// is refused ({"refused": true}), and so is one that names none to a side that
// keeps every plugin's.
[[nodiscard]] QJsonObject take(const Library &library, const QJsonObject &request, const QString &plugins = {},
                               Keeping keeping = Keeping::Installed);

// What each side holds, as survey() answers it.
struct Sides {
    QJsonObject here, there;
};
// Here, before the log goes: none when there knows nothing of plugins' files.
[[nodiscard]] std::optional<Sides> begin(const Library &library, const Remote &remote, const QString &plugins);
// Here, once it went: fetches and sends what each side lacks, counted in `summary`.
void finish(const Library &library, const Remote &remote, const QString &plugins, const Sides &sides, SyncSummary &summary);

// Puts an on-demand file of a plugin's into its data/ folder, from where the
// last sync saw it (PluginFiles::elsewhere): from the computer it names, or
// else from `syncTarget`, the one this computer syncs with, through `ssh`
// (Remote; the system's when empty). It must hold what the log says it does.
enum class Fetched : std::uint8_t {
    Here,     // it was here already
    Fetched,  // it is here now
    Nowhere,  // nothing says where it is, or there is no computer to ask
    Failed,   // it didn't arrive whole
};
[[nodiscard]] Fetched fetch(const Library &library, const QString &plugins, const QString &plugin, const QString &path,
                            const QString &syncTarget, const QStringList &ssh = {});

} // namespace PluginFileSync
