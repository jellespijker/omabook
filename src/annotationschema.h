#pragma once

#include <QStringList>

// The annotations table's formats (LibrarySchema's steps for it).
namespace AnnotationSchema {

// Highlights and margin notes on a passage (#80): the table made anew with
// where a passage ends, its words with a few before and after, the file its
// places were found in, how sure that was, and whether it is lost.
[[nodiscard]] QStringList passages();

} // namespace AnnotationSchema
