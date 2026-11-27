#pragma once

#include <QString>
#include <QStringList>
#include <expected>

#include "bookmeta.h"
#include "library.h"

// What becomes of a book's details when it is folded into another
// (Library::merge). The kept book takes what
// only the folded one has; where both say something else, the kept book's
// stays, and what is not kept is named before anyone agrees to it (#282).
namespace LibraryMerge {

// The kept book's details with what only the folded one has, and the tags of both.
[[nodiscard]] BookMeta details(const BookMeta &kept, const BookMeta &folded);
// What the folded book says that neither stays nor is carried over, by the
// names of BookMeta::toJson, and "rating" and "finished".
[[nodiscard]] QStringList dropped(const Book &kept, const Book &folded);
// The kept book as it is once the other is folded into it.
[[nodiscard]] Book after(const Book &kept, const Book &folded);
// Gives the kept book what the other's files and details bring it, before the
// other goes: the formats it lacks, a cover when it has none, and the details.
[[nodiscard]] std::expected<void, QString> carry(Library &library, const Book &kept, const Book &folded);

} // namespace LibraryMerge
