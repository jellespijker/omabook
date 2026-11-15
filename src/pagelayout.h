#pragma once

#include <QFont>
#include <QList>
#include <QString>

class QTextDocument;

// How a flowing book's text is laid out across the room it has: how long its
// lines are, and how many pages stand side by side.
//
// The longest line is a measure in characters, not in pixels, because that
// is what decides how a line reads: the eye takes in a line and has to find
// the start of the next, and both get harder as the count of characters
// grows, whatever the type's size. The typographic consensus (Bringhurst,
// The Elements of Typographic Style, 2.1.2) is that 45 to 75 characters,
// counting letters and spaces, read well on a single-column page, and that
// 66 is the ideal; Butterick's Practical Typography allows up to 90; WCAG
// 2.1 (1.4.8, Visual Presentation) asks for no more than 80. So the measure
// is 40 to 90 characters, 66 when not set, and is turned into pixels from
// the body font itself (characterWidth), so it stays the same count of
// characters at every size of type and in every font.
//
// Every page of a spread is a single-column page, so each keeps at least
// `minimumMeasure` characters; a window with room for more than one such
// page shows two or three, and none narrower than that.
namespace PageLayout {

// The fewest characters a page of a spread is set to (or the measure, when
// the reader asked for shorter lines than that): the shortest line
// Bringhurst counts as satisfactory on a page of its own.
inline constexpr int minimumMeasure = 45;
// What Automatic asks of each page before it adds one: most of the reader's
// own measure, so lines set long on purpose aren't cut short to make room
// for another page.
inline constexpr double automaticShare = 0.8;
// The most pages side by side. A spread of three already spans most of a
// wide screen; a fourth would be read with the head turned.
inline constexpr int mostColumns = 3;
// How many characters a page may lose, before the spread drops a page, once
// it has it (never going under the minimum measure). Without it a window
// resized across the boundary would flicker between one count and the next
// on every pixel.
inline constexpr double hysteresis = 3;

// The advance of an average character of running text in this font, in
// pixels: the widths of the lowercase letters weighted by how often each
// is met in English prose, and the space between words weighted by how
// often a word ends. It is what a count of characters is turned into
// pixels with. Without a window system (a command line) there are no
// fonts to ask, and it is half the type's size, which is near what book
// faces measure.
[[nodiscard]] double characterWidth(const QFont &font);
// The reader's body font: a family ("" for the desktop's) at a size in pixels.
[[nodiscard]] QFont bodyFont(const QString &family, int pixelSize);
// The space between two pages of a spread: three times the type's size, so
// it grows with the text it separates.
[[nodiscard]] double gutter(int fontSize) noexcept;
// A longest line that was given in pixels (before it was a measure), as the
// characters it held at a size of type, using half the size as the average
// character's width, within 40 to 90.
[[nodiscard]] int measureFromPixels(int pixels, int fontSize) noexcept;

// The room the text has and how it is to be set.
struct Space {
    double available = 0;      // the reading area's width, panes beside it already taken off
    double characterWidth = 9; // characterWidth() of the body font
    int measure = 66;          // the longest line, in characters
    double margin = 56;        // at each outer edge of the spread
    double gutter = 56;        // between two pages
};

// What fits in it.
struct Columns {
    int count = 1;          // the pages shown side by side
    int automatic = 1;      // what Automatic gives here
    int most = 1;           // the most that keep the minimum measure
    double width = 0;       // each page's text, in pixels: never past the measure
    bool honoured = true;   // a choice of 1, 2 or 3 is what shows
};

// The pages side by side. `wanted` is the reader's choice: 0 for
// Automatic, else 1 to 3, which is shown when each page keeps the minimum
// measure, and otherwise the most that do. Automatic shows as many pages
// as each keep `automaticShare` of the measure (and never less than the
// minimum). `previous` is the count shown until now (0 for none), which
// keeps its pages until they would lose `hysteresis` characters more.
[[nodiscard]] Columns columns(const Space &space, int wanted, int previous = 0);

// Where a chapter laid out as one column is cut into pages `height` tall:
// the top of each page in the column, and last where the column ends. Pages
// are cut between lines, never through one. Qt's text layout has no widow
// or orphan control of its own (only a paragraph's page-break-before and
// -after, and its lines kept together, which it applies when it prints), so
// they are kept here from the lines it laid out:
//
//   - a paragraph's first line is never left alone at the foot of a page,
//     nor its last line alone at the head of the next: the cut moves up a
//     line, or before the paragraph when that would leave one alone again;
//   - a heading is never the last thing on a page: it goes over with the
//     paragraph after it;
//   - a paragraph that asks to start (or end) a page does, and one whose
//     lines are to be kept together is when it fits on a page;
//   - a cut is moved only while the page keeps some of its lines; a page
//     that would be left empty is cut where the lines run out instead.
//
// A line taller than a page (a picture) gets a page of its own and is cut,
// which beats never showing its lower half.
[[nodiscard]] QList<double> pageBreaks(QTextDocument &document, double height);

} // namespace PageLayout
