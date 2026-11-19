#pragma once

#include <QJsonObject>
#include <QList>

#include "formats.h"

// The book formats the plugins Omabook knows of read, as the plugins'
// registry lists them (share/omabook/plugins.json, see pluginfinder.h):
//
//   "formats": [{"extension": "story", "name": "Storyteller story"}]
//
// made known to Formats at start-up, so that a book in one is kept where
// its plugin isn't installed, and says which plugin to install (#25).
namespace PluginFormats {

// The formats a registry's plugins say they read; an entry that isn't one is passed over.
[[nodiscard]] QList<PluginFormat> listed(const QJsonObject &registry);
// Tells Formats those of the registry compiled into this Omabook.
void know();

} // namespace PluginFormats
