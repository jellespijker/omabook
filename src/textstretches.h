#pragma once

#include <QString>
#include <vector>

#include "bookplace.h"

class BookText;

// A book's text cut into stretches for the meaning index (#72): runs of words
// of about the same length, each sharing a few words with the next so that
// what is said across a cut is whole in one of them. A stretch lies within one
// section, as a passage does, and its places count characters of that
// section's text (BookText), so the reader goes to it as to any place.
namespace TextStretches {

struct Stretch {
    BookPlace from;
    BookPlace to;
    QString text; // its words, their spacing made plain
};

// The length of a stretch in words, and how many of them the next one
// starts with: long enough to say something, short enough that an
// embedding model's few hundred tokens hold all of it.
inline constexpr int length = 160;
inline constexpr int shared = 40;

// The stretches of every section in turn; none for a section without words.
// `shared` is less than `length`.
[[nodiscard]] std::vector<Stretch> of(const BookText &text, int length = TextStretches::length, int shared = TextStretches::shared);

} // namespace TextStretches
