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

// What a line break in the text is. A flowing book breaks a line only where
// it means to (a heading, a line of verse), and that ends what is read. A
// page of a PDF breaks one wherever the line was full, mostly inside a
// sentence, often inside a word with a hyphen: there a line ends what is
// read only when it is well short of the page's longest (a heading, the end
// of a paragraph) and the next does not go on in small letters.
enum class Lines { End, Wrap };

// The sentences of a text in order, each without the space around it; a
// stretch with no letter or digit in it (a row of stars between scenes) is
// none.
[[nodiscard]] QList<Sentence> of(QStringView text, Lines lines = Lines::End);
// The words of one of them, as a voice is given them: of a page whose lines
// wrap, its lines joined by a space, and a word hyphenated over two whole.
[[nodiscard]] QString words(QStringView text, const Sentence &sentence, Lines lines = Lines::End);
// Which of them a character is in, or the first after it when it is
// between two; -1 past the last.
[[nodiscard]] qsizetype at(const QList<Sentence> &sentences, qsizetype offset);

} // namespace Sentences
