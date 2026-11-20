#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Words broken at the end of a line by the rules of their language, so that
// justified text isn't only filled out with ever wider spaces.
//
// Qt's text layout has no hyphenation of its own, but it does break a line
// at a soft hyphen (U+00AD) and shows a hyphen there. So the paragraphs of a
// chapter are given soft hyphens where their words may be broken before the
// chapter is laid out. Where they may be is found by Liang's patterns (Frank
// Liang, "Word Hy-phen-a-tion by Com-put-er", 1983), as TeX does it, from the
// hyphenation dictionaries (hyph_*.dic) LibreOffice and Hunspell use. Arch
// packages them as hyphen-en, hyphen-de, hyphen-nl and so on, in
// /usr/share/hyphen; a language without one is not hyphenated.
//
// The soft hyphens are only for the eye. Whatever finds words in a chapter
// as it is shown, or says where they are (a search, the graph's marks, a
// passage), reads it through `Plain`: the text without them, at the
// positions it has without them, so those are the same whether hyphenation
// is on or off. A soft hyphen the book had itself counts as none either.
namespace Hyphenation {

inline constexpr QChar softHyphen{0x00AD};

// One language's patterns, matched as libhyphen (the library LibreOffice
// hyphenates with) matches them, so a word is broken where it would be
// there. The German and some other dictionaries are written for its way:
// an automaton that, at each letter, applies the one pattern its state
// stands for, not every pattern ending there as Liang's lookup does; and
// two levels (NEXTLEVEL), the first finding where a compound's words meet,
// each of which is then broken again, the second the syllables of a word
// that is no compound.
class Dictionary {
public:
    // A dictionary file: its first line names its character set (UTF-8,
    // ISO8859-1, ...), then come patterns, one a line, and the fewest
    // letters to leave before and after a break (LEFTHYPHENMIN,
    // RIGHTHYPHENMIN, and the COMPOUND ones for the words of a compound).
    [[nodiscard]] static std::expected<Dictionary, QString> read(const QString &path);
    [[nodiscard]] static std::expected<Dictionary, QString> parse(const QByteArray &bytes);

    // Where a word may be broken: the positions in it (1 to its length less
    // one) a hyphen may go before, in order.
    [[nodiscard]] QList<qsizetype> points(QStringView word) const;
    // The word with a soft hyphen at each of them.
    [[nodiscard]] QString hyphenated(QStringView word) const;
    [[nodiscard]] int leftMin() const noexcept { return m_left; }
    [[nodiscard]] int rightMin() const noexcept { return m_right; }

private:
    // One level's patterns as an automaton: a state for every beginning of
    // a pattern, its letters' way on, where it falls back to (the longest
    // end of it that is a state too), and the pattern it completes, if any.
    struct Level {
        std::vector<std::pair<std::uint64_t, std::int32_t>> next; // (state, letter) to state, in order
        std::vector<std::int32_t> fallback;
        std::vector<std::uint32_t> matchAt; // into `values`
        std::vector<std::uint8_t> matchSize, values;
        [[nodiscard]] std::int32_t step(std::int32_t state, char16_t letter) const;
        // The places in a word (in lower case) whose value is odd.
        [[nodiscard]] std::vector<bool> breaks(std::u16string_view word) const;
    };
    using Patterns = std::unordered_map<std::u16string, std::vector<std::uint8_t>>;
    [[nodiscard]] static Level built(const Patterns &patterns);
    // A line of the file read into the levels; false at the end of what is read.
    bool take(QStringView line, std::vector<Patterns> &levels);
    [[nodiscard]] std::vector<bool> breaks(std::u16string_view word, bool start, bool end, int depth) const;

    std::vector<Level> m_levels;
    int m_left = 2, m_right = 2, m_compoundLeft = 2, m_compoundRight = 2;
};

// The folders dictionaries are looked for in: those in the environment's
// OMABOOK_HYPHENATION_PATH (separated by colons), then "hyphen" in each of
// the XDG data folders (~/.local/share/hyphen, /usr/share/hyphen).
[[nodiscard]] QStringList folders();
// The dictionary for a language as a book gives it ("en", "en-GB", "de_DE",
// "nld"): the one for its region when there is one, else the language's
// usual one (en_US, de_DE, nl_NL), else any for the language. None when no
// dictionary for it is installed. Each is read once and kept while the
// program runs, so the pointer stays good.
[[nodiscard]] const Dictionary *forLanguage(const QString &language);

// Some HTML with soft hyphens where the words of its text may be broken;
// never in a tag or an entity, nor in a word that has a soft hyphen of its
// own, is written in capitals, or is part of a number or an address
// (2nd, a/b, a@b.org, snake_case).
[[nodiscard]] QString hyphenateHtml(QStringView html, const Dictionary &dictionary);

// A chapter's text as shown, read without soft hyphens.
class Plain {
public:
    explicit Plain(QStringView shown);
    // The text without them.
    [[nodiscard]] const QString &text() const noexcept { return m_text; }
    // A position between two characters of the shown text, in the plain one.
    [[nodiscard]] qsizetype toPlain(qsizetype shown) const;
    // Where the character at a position of the plain text is in the shown
    // one; and where the end of a stretch of it that ends at `end` is.
    [[nodiscard]] qsizetype toShown(qsizetype plain) const;
    [[nodiscard]] qsizetype toShownEnd(qsizetype end) const;

private:
    QString m_text;
    // Where the soft hyphens are in the shown text, in order.
    QList<qsizetype> m_hyphens;
};

} // namespace Hyphenation
