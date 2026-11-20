#pragma once

#include <QCollator>
#include <QLocale>

// The order names are shown in: titles, authors, series, tags, a comic's pages,
// the graph's lists. One rule everywhere (#121): the desktop language's
// collation, numbers by value ("Vol 9" before "Vol 10") and case ignored.
// Under a C or POSIX locale (a bare container, a desktop set to C.UTF-8) the
// system collates by bytes, so the Unicode default (root) collation is used
// instead.
namespace Collation {

// The locale whose collation names follow: the given one, or the root
// collation's when that one is C or POSIX.
[[nodiscard]] QLocale localeFor(const QLocale &system = QLocale::system());

// A collator for names: numeric and case-insensitive, in localeFor(system).
[[nodiscard]] QCollator natural(const QLocale &system = QLocale::system());

} // namespace Collation
