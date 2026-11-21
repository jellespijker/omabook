#pragma once

#include <QStringList>

// The steps that change the table of the books' files (formats), each one
// that can be taken again: made anew whole, its index and triggers with it.
namespace FileSchema {

// The hash a file came in with, once the book's details were written into it
// (#74, format 41), kept beside the hash it has now (FormatFile::original).
[[nodiscard]] QStringList originals();

} // namespace FileSchema
