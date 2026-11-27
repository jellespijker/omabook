#pragma once

#include <QString>
#include <QtTypes>

#include "bookmeta.h"

// The folders a library keeps its books in, Author/Title (number), inside its
// own. One may be there already with an older library's files in it, as when a
// library is started over a folder whose database was lost (#281): what is in
// it is someone's, so nothing new is put there.
namespace BookFolder {

// The folder, within the library's, that a book of these details is named for.
[[nodiscard]] QString named(const BookMeta &meta, qint64 id);
// The folder, within `root`, a book just made is given: the one it is named
// for, or, where that holds something, the same with a number after it.
[[nodiscard]] QString forNew(const QString &root, const BookMeta &meta, qint64 id);
// Whether the folder holds a library's books, or the copies one kept of its
// database: it was a library, whatever its database says now.
[[nodiscard]] bool anyIn(const QString &root);

} // namespace BookFolder
