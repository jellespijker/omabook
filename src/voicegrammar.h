#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>
#include <optional>
#include <span>

// What a reader says to Omabook while reading (#150), understood without a
// model: "next chapter", "previous page", "highlight this", "read aloud",
// "go to chapter 5", "what does this word mean", in the five languages of
// the window (English, German, French, Dutch and Mexican Spanish). Each
// phrase stands for a command of the command table (CommandTable), or for
// one of the three only said, which take what was said with them: a
// chapter's number, the selection, or a word. A transcript is matched whole,
// after its case, accents, punctuation and a "please" are let go of, in the
// window's language first and then in the others.
namespace VoiceGrammar {

// The commands only said, beyond the table's: a chapter by its number, the
// selected words highlighted, and what a word means (the one said, or the
// one selected).
inline constexpr auto chapter = "spoken.chapter";
inline constexpr auto highlight = "spoken.highlight";
inline constexpr auto meaning = "spoken.meaning";

struct Heard {
    QString command;  // a command of the table, or one of the three above
    int number = 0;   // the chapter's, for `chapter`
    QString words;    // the word asked about, for `meaning`; empty for the selection's
    QString language; // the language it was understood in: "en", "de", "fr", "nl" or "es"
};

// The languages it knows, as the window names them, short ("es" for es-MX).
[[nodiscard]] QStringList languages();
// Every command a phrase stands for.
[[nodiscard]] QStringList commands();

// What `said` stands for, tried in `language` (the window's, "de" or "es-MX")
// first, then in each of the others; nothing when no phrase is it.
[[nodiscard]] std::optional<Heard> understand(QStringView said, QStringView language);
// The same, in `language` alone.
[[nodiscard]] std::optional<Heard> understandIn(QStringView said, QStringView language);

// `said` as the phrases are matched against: lower case, without accents,
// punctuation, or a "please" (in any of the languages), one space between words.
[[nodiscard]] QString normal(QStringView said);
// A number said in `language`, in figures or in words ("5", "five", "fünf",
// "twenty one"); nothing for any other words.
[[nodiscard]] std::optional<int> number(QStringView said, QStringView language);

// An entry of a book's contents, as Reader::toc has it.
struct Entry {
    QString title;
    int level = 0;
};
// Which entry of `contents` is chapter `number`: the first whose title
// begins with that number, in figures or Roman ("Chapter 5", "5. The Ferry",
// "V"); else the number-th of the top level's. -1 when there is none.
[[nodiscard]] int chapterIn(std::span<const Entry> contents, int number);

} // namespace VoiceGrammar
