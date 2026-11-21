#pragma once

#include <QList>
#include <QString>

#include "privatebooks.h"

class Library;

// What the plugins in a folder say of the books they keep private (#127),
// in their plugin.json:
//
//   {"name": "Notebooks", ..., "private": ["omanotebook"]}
//
// each the extension of a format of the plugin's own, read whether the
// plugin is switched on or not, and said once in the library's change log
// (PrivateBooks::declare), where it stays after the plugin is removed.
namespace PluginPrivacy {

// The kinds the plugins in `folder` keep private.
[[nodiscard]] QList<PrivateBooks::Kind> listed(const QString &folder);

// Says each of those in the library that wasn't said yet. How many were news.
qsizetype record(const Library &library, const QString &folder);

} // namespace PluginPrivacy
