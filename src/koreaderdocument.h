#pragma once

#include <QString>
#include <QStringView>
#include <QtTypes>
#include <optional>
#include <span>

#include "bookplace.h"

// A book as KOReader, the e-reader app, knows it (#71): by the hash of its
// file, and its reading place by what KOReader's progress sync says of it.
namespace KoreaderDocument {

// The document hash KOReader's progress sync names a book by: the MD5 of
// 1 KiB read at each of 0, 1 KiB, 4 KiB, 16 KiB and so on by fours up to
// 1 GiB, as far as the file goes (KOReader's util.partialMD5). Empty for a
// file that can't be read.
[[nodiscard]] QString hash(const QString &path);

// Whether KOReader shows a book of this format as pages, and says its place
// by a page number, rather than by a place in its text.
[[nodiscard]] bool paged(const QString &format);

// The place KOReader's progress says in a book of `format`: a page number
// (from 1) for a paged book; for a flowing one an XPointer, whose
// DocFragment (an EPUB's spine item) or top-level section (a FictionBook's)
// is the section, and how far into it is told by `percentage` (0 to 1, of
// the whole book) weighed by `sectionSizes`, the length of each section's
// text. A progress that says no section is placed by the percentage alone.
[[nodiscard]] BookPlace placeOf(QStringView progress, double percentage, const QString &format, std::span<const qsizetype> sectionSizes);

// What KOReader is told of a place: a page number, or an XPointer to the
// start of its section. None for a format whose places KOReader can't be
// pointed at that way (plain text, Markdown).
[[nodiscard]] std::optional<QString> progressOf(const BookPlace &place, const QString &format);

} // namespace KoreaderDocument
