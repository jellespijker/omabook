#pragma once

#include <QString>

// Whether a library is where it is looked for, and whether one is expected
// there (#221). A drive not connected, a share not mounted or a folder moved
// away leaves nothing at a library's path, or the empty folder it was mounted
// on. Where a library is expected, Library::open makes no new one in its place
// unless that was asked for (Library::createIfMissing): the person would be
// left with two libraries of one name, one of them hidden under a mount.
//
// One is expected where config.json names it (the one in use, or one of the
// list), and where this program opened one before. While config.json can't be
// read, which it names isn't known, so one is expected in any folder: the
// usual one would otherwise take the place of the person's own.
namespace LibraryPresence {

// Whether the folder holds a library now.
[[nodiscard]] bool there(const QString &folder);
// Whether config.json names the folder, or may: it can't be read.
[[nodiscard]] bool configured(const QString &folder);
// Not there, though one is expected.
[[nodiscard]] bool missing(const QString &folder);
// Remembers that a library was opened there. The usual folder, which
// config.json doesn't name until another is chosen, is written down in it, so
// the next program knows of it too.
void opened(const QString &folder);

} // namespace LibraryPresence
