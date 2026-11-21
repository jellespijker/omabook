#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QtTypes>

class Library;

// The files of a plugin's data/ folder as they travel to one's other
// computers (#131), as a book's files do: by their content, once. Each is
// recorded in the plugin's own space of the change log (PluginData, ADR 0002)
// as a thing of the entity `omabook-file`, by its path under data/, with the
// hash and size of what it holds; a sync carries the records, and then each
// file one side lacks, checked against its hash when it arrives.
//
// What a computer has is recorded at a sync (note), so a file a plugin wrote
// any way at all travels: with omabook.files, as a recording, from its view.
// A removal is recorded as the plugin makes it (omabook.files.remove), with
// what the file held, and the file goes on the other computers too, unless it
// changed there since. A file that is gone without that is fetched back.
//
// Like everything of a plugin's space, the records go to one's own computers
// only, never into a shared or published space: a part of a space for a
// plugin's data (#127) is where that would change.
//
// Held to what a plugin may keep: a file of at most maxFileBytes, and of one
// plugin's files at most maxPluginBytes, the first by path; what is over stays
// where it is, unrecorded. settings.json (omabook.settings) stays on its computer.
namespace PluginFiles {

inline constexpr qint64 maxFileBytes = qint64{64} * 1024 * 1024;
inline constexpr qint64 maxPluginBytes = qint64{1024} * 1024 * 1024;
inline constexpr auto entity = "omabook-file";

struct File {
    QString hash;
    qint64 size = 0;
    bool removed = false; // the log says it was removed; `hash` is what it held then
};
// By the path under data/.
using Files = QHash<QString, File>;

// Whether a path under data/ is one that travels: relative, of names that
// are no "." or "..", and not settings.json.
[[nodiscard]] bool travels(const QString &path);
// `path` under `dataFolder` as a real path, links followed, or empty when it
// would lead out of the folder.
[[nodiscard]] QString inside(const QString &dataFolder, const QString &path);
// How many bytes the files under `dataFolder` hold together.
[[nodiscard]] qint64 used(const QString &dataFolder);

struct Survey {
    Files files;      // what travels, within the limits
    QStringList held; // the paths over them
};
// What is in a plugin's data folder now.
[[nodiscard]] Survey survey(const QString &dataFolder);

// What the log says of a plugin's files, the removed ones included.
[[nodiscard]] Files recorded(const Library &library, const QString &plugin);
// Records each file of `disk` the log doesn't say has that content, unless
// the log says it was removed with that content (on another computer, so it
// goes here too). Answers how many it recorded.
int note(const Library &library, const QString &plugin, const Files &disk);
// Records that the plugin removed a file that held `hash`.
bool forget(const Library &library, const QString &plugin, const QString &path, const QString &hash);
// Removes each file of the data folder the log says was removed, while it
// still holds what was removed. Answers how many.
int settle(const Library &library, const QString &plugin, const QString &dataFolder);
// Puts `file` into the data folder as `path`: only when the log says that
// path holds its content, and within the limits.
[[nodiscard]] bool take(const Library &library, const QString &plugin, const QString &dataFolder, const QString &path, const QString &file);

} // namespace PluginFiles
