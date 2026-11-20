#pragma once

#include <QHash>
#include <QList>
#include <QStringView>
#include <cstddef>
#include <optional>

// Finding a passage's words again in a text that may have changed since they
// were taken from it (#80): a new edition, a typo fixed, a sentence put in or
// taken out. Texts are compared by their words, so line breaks, spacing,
// case and soft hyphens never matter, and where the words occur more than
// once, the words around them and the place they were at tell which.
namespace PassageMatch {

// One word of a text: where it starts and ends in it, and what it is, its
// case folded (a hash, compared, never shown).
struct Word {
    qsizetype start = 0;
    qsizetype end = 0;
    size_t key = 0;
};

// The words of a text in order. A word is a run of letters and digits; in
// writing without spaces (Chinese, Japanese, Thai) each character is one.
[[nodiscard]] QList<Word> words(QStringView text);

// A text's words and where each of them occurs: made once for a section,
// then asked about each passage sought in it.
class Index {
public:
    explicit Index(QStringView text);
    [[nodiscard]] const QList<Word> &words() const noexcept { return m_words; }
    [[nodiscard]] qsizetype length() const noexcept { return m_length; }
    // The positions (in words()) where a word occurs, first to last.
    [[nodiscard]] QList<int> occurrences(size_t key) const { return m_where.value(key); }

private:
    QList<Word> m_words;
    QHash<size_t, QList<int>> m_where;
    qsizetype m_length = 0;
};

// What is looked for: the passage's words, a few just before and after, and
// how far into the text it was (0 to 1).
struct Sought {
    QStringView quote;
    QStringView before;
    QStringView after;
    double near = 0;
};

// Where it was found, in characters of the text, and how sure that is: how
// many of its words are there (1 when all are, in order), how many of the
// words around them, and the two together (`confidence`, 0 to 1). `distance`
// is how far that is from where it was (0 to 1).
struct Match {
    qsizetype start = 0;
    qsizetype end = 0;
    double words = 0;
    double context = 0;
    double confidence = 0;
    double distance = 0;
    // Its words all there and the words around them too: the passage itself.
    [[nodiscard]] bool sure() const noexcept { return words >= 1 && context >= 1; }
};

// The best place of it in the text, or none when too few of its words are
// there to say it is the passage.
[[nodiscard]] std::optional<Match> find(const Index &index, const Sought &sought);

} // namespace PassageMatch
