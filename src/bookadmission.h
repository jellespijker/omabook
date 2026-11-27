#pragma once

#include <QString>
#include <expected>
#include <optional>

#include "library.h"

// A file coming in from outside, from a person, an agent or a plugin, is
// looked at first (#226): one that is empty, damaged or cut short is no book
// and doesn't come in, and one that is protected comes in with a word that it
// can't be read here. What another library took already (a sync, a shared
// space, a merge) is carried as it is, and so is a file this library holds.
namespace BookAdmission {

// Why a file that may come in can't be read here, if it can't; or why it
// may not come in.
using Verdict = std::expected<std::optional<Failure>, Failure>;

// The verdict on a file of hash `hash`. What Omabook reads itself is opened,
// what it converts is looked at (BookSignature); a file that is no book, or
// none to be read at all, is left for Library::add and addFormat to refuse in
// their own words.
[[nodiscard]] Verdict examine(const Library &library, const QString &file, const QString &hash);

// Library::add, for a file that may come in; the error says why one may not.
[[nodiscard]] Library::Added add(Library &library, const QString &file, const BookMeta &meta);

// Library::addFormat, for a file that may come in.
[[nodiscard]] Verdict addFormat(Library &library, qint64 id, const QString &file, Library::SameFormat same);

} // namespace BookAdmission
