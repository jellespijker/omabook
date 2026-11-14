#pragma once

#include <QString>

#include "settings.h"

// Taking a library off the list of the ones this computer knows, for the
// window and the command line alike.
namespace Libraries {

// Whether a folder is a Calibre library that Omabook was pointed at as if it
// were its own: Calibre's database is there, and Omabook's holds no books.
// Such a folder is to be imported from, not opened.
bool isCalibreOnly(const QString &folder);

// Forgets a library: it leaves the list and its folder stays. The library in
// use can only be forgotten when there is another to use instead, which then
// becomes the one in use. An Omabook database that holds nothing, left in a
// folder of other things (a Calibre library opened by mistake), is tidied away.
bool forget(Settings &settings, const QString &folder, QString *error = nullptr);

// Forgets a library and moves its folder to the trash. Refused for a folder
// that is also a Calibre library, which is not Omabook's to throw away.
bool trash(Settings &settings, const QString &folder, QString *error = nullptr);

} // namespace Libraries
