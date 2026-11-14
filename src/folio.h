#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include "settings.h"

// Omabook's side of its plugins for Folio, Omarchy's file manager. Folio
// runs commands and reads the JSON they print (see its plugins.md); these
// are those commands' answers, so the manifests in share/folio only name
// `omabook folio …` and hold no logic of their own.
namespace Folio {

// The poll of the "omabook" plugin: each library as an item that opens in
// Omabook, with how many books it holds.
QJsonObject libraries(const Settings &settings);
// The poll of the "omabook-reading" plugin: the books being read in the
// current library, the one read last first.
QJsonObject reading(const Settings &settings);
// The rows Folio's details pane shows for a book file: which library has
// it and how far it was read. Empty for a file no library has.
QJsonArray details(const Settings &settings, const QString &file);
// Adds files and folders to a library (the current one when `library` is
// empty); the result is what Folio shows: a notice, or an error.
QJsonObject add(const Settings &settings, const QString &library, const QStringList &paths);

// `omabook folio <libraries|reading|details|add|open|open-library> …`
int run(const QStringList &arguments);

} // namespace Folio
