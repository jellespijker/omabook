#pragma once

#include <QString>
#include <expected>
#include <optional>

#include "failure.h"

// What a file of a format Omabook has others read (it converts it) shows of
// being one, as it comes in (#264): how it begins, and for a document that it
// ends. Only what can be told without reading it as its format would is looked
// at, so a file is never turned away on a guess.
namespace BookSignature {

// Why a file of the format `extension` can't be read here, if it plainly
// can't; or why it is no such file at all.
using Verdict = std::expected<std::optional<Failure>, Failure>;

// The verdict on the file at `path`; nothing is said of a format not looked at.
[[nodiscard]] Verdict look(const QString &path, const QString &extension);

} // namespace BookSignature
