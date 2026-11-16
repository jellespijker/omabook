#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <array>
#include <span>

// How the text of a flowing book is set on the page: the type, the measure,
// the spacing and the alignment. The library's defaults are in config.json
// (see Settings); a book can have its own for any of them, kept with the
// book in the library (see Library::bookFormat), which lie over the
// defaults. Pages that are pictures (PDF, comics) are drawn as they are and
// none of this applies to them.
//
// What Qt's text layout does with it, which is what is offered and no more:
// line spacing, the space after a paragraph, a first-line indent and the
// alignment of a paragraph are block formats it lays out itself. Justified
// text is filled with wider spaces, never letter spacing. It has no
// hyphenation of its own (no dictionaries, no option for it), so there is
// no setting for that; it does break a line at a soft hyphen a book already
// has in its text.
namespace ReadingFormat {

using namespace Qt::StringLiterals;

// A setting that is a whole number, held between `low` and `high`, which
// keep any value of it readable.
struct Range {
    QLatin1StringView key;  // as config.json, the database and the tools name it
    int low, high, fallback;
    int step;               // what one press of a stepper changes it by
    QLatin1StringView unit; // "px", "%" or "characters"
};

inline constexpr std::array ranges{
    Range{"font_size"_L1, 10, 48, 19, 1, "px"_L1},
    // The longest line, in characters of the body font (see pagelayout.h).
    Range{"measure"_L1, 40, 90, 66, 2, "characters"_L1},
    Range{"line_height"_L1, 100, 300, 150, 10, "%"_L1},
    Range{"paragraph_spacing"_L1, 0, 80, 12, 2, "px"_L1},
    Range{"indent"_L1, 0, 64, 0, 4, "px"_L1},
    Range{"margins"_L1, 0, 160, 32, 8, "px"_L1},
};

// Where the lines of a paragraph go: against the side the text starts at
// (left in English, right in Arabic), filled out to both sides, centred, or
// against the side it ends at. "start" and "end" turn round with the
// direction of the text, paragraph by paragraph.
inline constexpr std::array alignments{"start"_L1, "justify"_L1, "center"_L1, "end"_L1};

// How a book is laid out to be read through. "pages": cut into pages that
// turn, one or a spread of two or three, each chapter starting on a page of
// its own; "scroll": one column that runs on from one chapter into the next;
// "auto": pages where the window has room for more than one side by side,
// else a column that scrolls (and pages that are pictures scroll). It
// applies to pages that are pictures too: in "pages" each is shown whole.
inline constexpr std::array layouts{"auto"_L1, "pages"_L1, "scroll"_L1};
// Pages that are pictures, two side by side: whether the first stands alone
// ("alone", as a printed book opens, so its left and right pages are where
// print put them: 1, 2|3, 4|5), or pairs from the start ("paired": 1|2,
// 3|4). "auto" is alone for a PDF whose first page is taller than it is
// wide, as a book's or a paper's is, and paired for slides, which are
// wider, and for comics: their archives most often hold a spread drawn
// across two pages as one wide picture already, and many begin without a
// cover of their own, so pairing from the first shows the most.
inline constexpr std::array firstPages{"auto"_L1, "alone"_L1, "paired"_L1};

// A setting that is one of a few words.
struct Choice {
    QLatin1StringView key;
    std::span<const QLatin1StringView> words;
    QLatin1StringView fallback;
};
inline constexpr std::array choices{
    Choice{"align"_L1, alignments, "start"_L1},
    Choice{"layout"_L1, layouts, "auto"_L1},
    Choice{"first_page"_L1, firstPages, "auto"_L1},
};
[[nodiscard]] const Choice *choice(QAnyStringView key);

// Every option by its key: the numbers above, then "font_family" (a font's
// name, empty for the desktop's), "align", "layout" and "first_page".
[[nodiscard]] QStringList keys();
[[nodiscard]] const Range *range(QAnyStringView key);
// Why a value can't be given to an option, in English, with what it may be;
// empty when it can.
[[nodiscard]] QString problem(const QString &key, const QJsonValue &value);
// Only the options among these that are options and fit; what an older or
// hand-edited record holds besides is left out.
[[nodiscard]] QJsonObject fitting(const QJsonObject &options);
// The options in force: a book's own laid over the defaults.
[[nodiscard]] QJsonObject over(const QJsonObject &defaults, const QJsonObject &own);
// What each number may be, for a window or a tool to show:
// {"font_size": {"low": 10, "high": 48, "default": 19, "step": 1, "unit": "px"}, ...,
//  "align": {"choices": [...], "default": "start"}, "layout": ..., "first_page": ...}.
[[nodiscard]] QJsonObject bounds();
// A chapter's cleaned HTML (see htmlclean.h) set with these options: the
// style sheet in front of it, and its paragraphs given what a style sheet
// can't give them in Qt (justification and the indent, which Qt only takes
// from the paragraph itself). Paragraphs the book aligned itself, such as
// a centred line, keep their own alignment and get no indent; nor does the
// first paragraph after a heading, as in a printed book.
[[nodiscard]] QString setChapter(const QString &html, const QJsonObject &options, const QColor &links);

} // namespace ReadingFormat
