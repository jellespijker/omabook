#include "pagelayout.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <ranges>
#include <vector>

namespace PageLayout {

namespace {

// How often each lowercase letter is met in English text, in percent of the
// letters (Lewand, Cryptological Mathematics, 2000).
constexpr std::array<double, 26> letterShare{8.167, 1.492, 2.782, 4.253, 12.702, 2.228, 2.015, 6.094, 6.966,
                                             0.153, 0.772, 4.025, 2.406, 6.749,  7.507, 1.929, 0.095, 5.987,
                                             6.327, 9.056, 2.758, 0.978, 2.360,  0.150, 1.974, 0.074};
// An English word is 4.7 letters long on average, so one character in 5.7
// is the space after one.
constexpr double spaceShare = 1 / 5.7;

} // namespace

double characterWidth(const QFont &font) {
    const double size = font.pixelSize() > 0 ? font.pixelSize() : font.pointSizeF() * 96 / 72;
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return size / 2;
    const QFontMetricsF metrics(font);
    double letters = 0, shares = 0;
    for (std::size_t index = 0; index < letterShare.size(); ++index) {
        letters += metrics.horizontalAdvance(QChar(char16_t(u'a' + index))) * letterShare[index];
        shares += letterShare[index];
    }
    const double width = (1 - spaceShare) * letters / shares + spaceShare * metrics.horizontalAdvance(u' ');
    return width > 0 ? width : size / 2;
}

QFont bodyFont(const QString &family, int pixelSize) {
    QFont font = qobject_cast<QGuiApplication *>(QCoreApplication::instance()) ? QGuiApplication::font() : QFont();
    if (!family.isEmpty())
        font.setFamily(family);
    font.setPixelSize(std::max(1, pixelSize));
    return font;
}

double gutter(int fontSize) noexcept { return std::clamp(3.0 * fontSize, 40.0, 120.0); }

int measureFromPixels(int pixels, int fontSize) noexcept {
    return std::clamp(static_cast<int>(std::lround(pixels / (std::max(1, fontSize) / 2.0))), 40, 90);
}

namespace {

// The pages side by side in the room, with no margin column.
Columns pagesIn(const Space &space, int wanted, int previous) {
    const double character = std::max(1.0, space.characterWidth);
    const int measure = std::max(1, space.measure);
    // The width each of `count` pages gets, in characters.
    const auto perPage = [&](int count) {
        return (space.available - 2 * space.margin - (count - 1) * space.gutter) / count / character;
    };
    const double lowest = std::min(minimumMeasure, measure);
    // Whether `count` pages keep `least` characters each; the pages shown
    // until now keep theirs down to a few characters less, but never under
    // the minimum.
    const auto fits = [&](int count, double least) {
        return count == 1 || perPage(count) >= std::max(lowest, least - (count == previous ? hysteresis : 0));
    };
    const auto mostKeeping = [&](double least) {
        int count = mostColumns;
        while (count > 1 && !fits(count, least))
            --count;
        return count;
    };
    const double comfortable = std::clamp(std::round(measure * automaticShare), lowest, double(measure));

    Columns result;
    result.most = mostKeeping(lowest);
    result.automatic = mostKeeping(comfortable);
    if (wanted >= 1 && wanted <= mostColumns) {
        result.honoured = fits(wanted, lowest);
        result.count = result.honoured ? wanted : result.most;
    } else {
        result.count = result.automatic;
    }
    result.width = std::max(0.0, std::min(measure * character, perPage(result.count) * character));
    return result;
}

// The margin column's width in this room, or 0 when a comfortable page
// doesn't fit beside it.
double noteColumn(const Space &space) {
    if (!space.notes)
        return 0;
    const double character = std::max(1.0, space.characterWidth);
    const double width = std::max(leastNoteWidth, noteMeasure * noteScale * character);
    const double lowest = std::min(minimumMeasure, std::max(1, space.measure));
    const double comfortable = std::max(lowest, std::round(space.measure * automaticShare)) * character;
    return space.available - 2 * space.margin - width - space.gutter >= comfortable ? width : 0;
}

} // namespace

Columns columns(const Space &space, int wanted, int previous) {
    const double notes = noteColumn(space);
    Space text = space;
    if (notes > 0)
        text.available -= notes + space.gutter;
    Columns result = pagesIn(text, wanted, previous);
    result.notes = notes;
    return result;
}

QList<double> stack(const QList<Note> &notes, double spacing, double bottom) {
    QList<qsizetype> order(notes.size());
    std::ranges::iota(order, qsizetype{0});
    std::ranges::stable_sort(order, {}, [&notes](qsizetype index) { return notes.at(index).top; });
    QList<double> tops(notes.size());
    // Down from the first: each where it wants, or below the one before.
    double free = 0;
    for (const qsizetype index : std::as_const(order)) {
        tops[index] = std::max(notes.at(index).top, free);
        free = tops.at(index) + notes.at(index).height + spacing;
    }
    if (bottom < 0)
        return tops;
    // Up from the last, within the foot: each above the one after, none above the top.
    double limit = bottom;
    for (const qsizetype index : std::views::reverse(order)) {
        tops[index] = std::max(0.0, std::min(tops.at(index), limit - notes.at(index).height));
        limit = tops.at(index) - spacing;
    }
    // Pushed against the top, they keep apart, and what doesn't fit runs past the foot.
    free = 0;
    for (const qsizetype index : std::as_const(order)) {
        tops[index] = std::max(tops.at(index), free);
        free = tops.at(index) + notes.at(index).height + spacing;
    }
    return tops;
}

namespace {

// One line of a laid-out chapter, with what a cut needs to know of the
// paragraph it is in.
struct Line {
    double top, height;
    int index;       // which of its lines
    int count;       // how many it has
    bool heading;    // the paragraph is a heading
    bool together;   // its lines are to be kept together
    bool before;     // it starts a page
    bool after;      // it ends one
    bool blank;      // nothing on it is seen: spaces, or nothing at all
    [[nodiscard]] double bottom() const noexcept { return top + height; }
};

// Every line of the chapter, top to bottom.
std::vector<Line> laidLines(QTextDocument &document) {
    std::vector<Line> lines;
    QAbstractTextDocumentLayout *layout = document.documentLayout();
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QRectF bounds = layout->blockBoundingRect(block);
        const QTextBlockFormat format = block.blockFormat();
        const QTextLayout *laid = block.layout();
        const QString text = block.text();
        for (int index = 0; index < laid->lineCount(); ++index) {
            const QTextLine line = laid->lineAt(index);
            // A picture is a character of its own, which is not a space.
            const bool blank = QStringView(text).mid(line.textStart(), line.textLength()).trimmed().isEmpty();
            lines.push_back({bounds.top() + line.y(), line.height(), index, laid->lineCount(), format.headingLevel() > 0,
                             format.nonBreakableLines(), bool(format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore),
                             bool(format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysAfter), blank});
        }
    }
    return lines;
}

} // namespace

QList<double> pageBreaks(QTextDocument &document, double height) {
    // Whole pixels: a page shown from a fraction of one is drawn a little
    // off where the next page's top was measured, and a sliver of a line
    // shows twice.
    height = std::floor(height);
    if (height <= 0)
        return {0.0, 0.0};
    const std::vector<Line> lines = laidLines(document);
    const double end = std::ceil(document.documentLayout()->documentSize().height());
    QList<double> breaks{0.0};
    if (lines.empty())
        return breaks << std::max(1.0, end);

    // The first line of the paragraph that line `at` is in.
    const auto blockStart = [&lines](std::size_t at) { return at - lines[at].index; };
    // Where the page that starts at line `start` is best cut, given that
    // line `over` is the first that doesn't fit on it; a cut is the index
    // of the line that starts the next page.
    const auto bestCut = [&](std::size_t start, std::size_t over) {
        std::size_t cut = over;
        const Line &first = lines[cut];
        // The paragraph is cut through: neither part may be one line, and
        // one whose lines are kept together isn't cut at all.
        if (first.index > 0) {
            const int kept = first.index, carried = first.count - first.index;
            if (first.together)
                cut = blockStart(cut);
            else if (carried == 1 && kept >= 3)
                cut -= 1;
            else if (carried == 1 || kept == 1)
                cut = blockStart(cut);
        }
        // A heading goes over with what follows it, and so does any
        // heading just before that one (a part title over a chapter's).
        while (cut > start && lines[cut - 1].heading)
            cut = blockStart(cut - 1);
        // Never at the cost of the whole page.
        return cut > start ? cut : over;
    };
    // The pixel row a page starts at, when line `cut` starts it: the last
    // at or above the line's top that is below the whole of the line
    // before, so that line is all on the page before and none of it on
    // this one, this line is all on this one, and the space between
    // paragraphs isn't carried over to the head of the page.
    // (The space between the two can be more than what is left of the
    // page before, after a picture set with the lines' spacing: the page
    // shows no more than `height` of it, and the next starts at its line.)
    const auto edge = [&lines](std::size_t cut) {
        const double top = lines[cut].top, before = lines[cut - 1].bottom();
        if (std::floor(top) >= before)
            return std::floor(top);
        return std::ceil(before) <= top ? std::ceil(before) : std::round(top);
    };

    std::size_t start = 0;
    double pageTop = 0;
    for (std::size_t at = 0; at < lines.size(); ++at) {
        const Line &line = lines[at];
        // A paragraph that asks for a page of its own begins one.
        const bool forced = at > start && line.index == 0
                            && (line.before || (lines[at - 1].after && lines[at - 1].index == lines[at - 1].count - 1));
        if (forced || line.bottom() - pageTop > height) {
            if (at > start) {
                const std::size_t cut = forced ? at : bestCut(start, at);
                pageTop = edge(cut);
                breaks << pageTop;
                start = cut;
                at = cut;
            }
            // A line taller than a page: cut through it, a page at a time.
            while (lines[at].bottom() - pageTop > height) {
                pageTop += height;
                breaks << pageTop;
            }
        }
    }
    // Pages with nothing on them to see, after the last that has (empty
    // paragraphs at a chapter's end), are no pages: the chapter's last
    // page holds what is left, and the next chapter starts on the next.
    const auto inked = std::ranges::find_if(lines.rbegin(), lines.rend(), [](const Line &line) { return !line.blank; });
    if (inked != lines.rend())
        while (breaks.size() > 1 && breaks.last() >= inked->bottom())
            breaks.removeLast();
    return breaks << std::min(breaks.last() + height, std::max(breaks.last() + 1, end));
}

double screenStep(QTextDocument &document, double top, double height, bool forward) {
    const std::vector<Line> lines = laidLines(document);
    const double fallback = forward ? top + height : std::max(0.0, top - height);
    if (lines.empty() || height <= 0)
        return fallback;
    if (forward) {
        // The first line that isn't wholly in sight at the foot of the window.
        const auto cut = std::ranges::find_if(lines, [&](const Line &line) { return line.bottom() > top + height + 0.5; });
        return cut != lines.end() && cut->top > top + 0.5 ? cut->top : fallback;
    }
    // The last line that isn't wholly in sight at the head of the window
    // ends the window before, which starts at the first line that fits.
    const auto above = std::ranges::find_if(lines.rbegin(), lines.rend(), [&](const Line &line) { return line.top < top - 0.5; });
    if (above == lines.rend())
        return 0;
    const double bottom = above->bottom();
    const auto first = std::ranges::find_if(lines, [&](const Line &line) { return line.top >= bottom - height - 0.5; });
    return first != lines.end() && first->top < top - 0.5 ? std::max(0.0, first->top) : fallback;
}

} // namespace PageLayout
