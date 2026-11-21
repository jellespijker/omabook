#pragma once

#include <QList>
#include <QStringView>
#include <QtTypes>

// A text cut into the sentences it is read aloud in (#73): where each starts
// and ends in it, so the one being read can be marked on the page and the
// place kept as it goes. The cuts are Unicode's (UAX #29, as Qt finds them),
// in any language; a paragraph is never part of the next one's sentence, and
// a sentence too long to wait for (a list run together, a page of a PDF
// without stops) is cut again where it pauses, at a comma or a space.
namespace Sentences {

struct Sentence {
    qsizetype start = 0;
    qsizetype end = 0; // the character past its last

    constexpr bool operator==(const Sentence &) const = default;
};

// The longest a sentence is read in one go, in characters.
inline constexpr qsizetype longest = 400;

// The sentences of a text in order, each without the space around it; a
// stretch with no letter or digit in it (a row of stars between scenes) is
// none.
[[nodiscard]] QList<Sentence> of(QStringView text);
// Which of them a character is in, or the first after it when it is
// between two; -1 past the last.
[[nodiscard]] qsizetype at(const QList<Sentence> &sentences, qsizetype offset);

} // namespace Sentences
