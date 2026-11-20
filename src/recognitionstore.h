#pragma once

#include <QString>
#include <expected>
#include <optional>

#include "recognition.h"

class Library;

// Recognised text and transcripts kept as data in the change log (#84), by
// the key of what was recognised (Recognition::key), so they go to one's other
// computers at a sync: only the computer that wrote the ink or made the
// recording has it recognised, and search finds the same text everywhere.
//
// A result is kept with its owner's data (PluginData, ADR 0002): a plugin's
// in its own space, as a thing of the entity `omabook-recognized`. Its
// segments, which for hours of speech are far more than one write may hold,
// go into the set `parts` a few dozen kilobytes at a time; the field `result`
// says how many there are, and is written last, so a result is read only whole.
namespace RecognitionStore {

inline constexpr auto entity = "omabook-recognized";

[[nodiscard]] std::expected<void, QString> keep(const Library &library, const QString &owner, const QString &key,
                                                const Recognition::Result &result);
// The result kept under the key, whole; none when there is none (or only part).
[[nodiscard]] std::optional<Recognition::Result> find(const Library &library, const QString &owner, const QString &key);

} // namespace RecognitionStore
