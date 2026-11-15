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

Columns columns(const Space &space, int wanted, int previous) {
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

QList<double> pageBreaks(QTextDocument &document, double height) {
    if (height <= 0)
        return {0.0, 0.0};
    // Every line of the chapter, in order, with what the cut needs to know
    // of the paragraph it is in.
    struct Line {
        double top, height;
        int index;       // which of its lines
        int count;       // how many it has
        bool heading;    // the paragraph is a heading
        bool together;   // its lines are to be kept together
        bool before;     // it starts a page
        bool after;      // it ends one
    };
    std::vector<Line> lines;
    QAbstractTextDocumentLayout *layout = document.documentLayout();
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QRectF bounds = layout->blockBoundingRect(block);
        const QTextBlockFormat format = block.blockFormat();
        const QTextLayout *laid = block.layout();
        for (int index = 0; index < laid->lineCount(); ++index) {
            const QTextLine line = laid->lineAt(index);
            lines.push_back({bounds.top() + line.y(), line.height(), index, laid->lineCount(), format.headingLevel() > 0,
                             format.nonBreakableLines(), bool(format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore),
                             bool(format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysAfter)});
        }
    }
    const double end = layout->documentSize().height();
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

    std::size_t start = 0;
    double pageTop = 0;
    for (std::size_t at = 0; at < lines.size(); ++at) {
        const Line &line = lines[at];
        // A paragraph that asks for a page of its own begins one.
        const bool forced = at > start && line.index == 0
                            && (line.before || (lines[at - 1].after && lines[at - 1].index == lines[at - 1].count - 1));
        if (forced || line.top + line.height - pageTop > height) {
            if (at > start) {
                const std::size_t cut = forced ? at : bestCut(start, at);
                pageTop = lines[cut].top;
                breaks << pageTop;
                start = cut;
                at = cut;
            }
            // A line taller than a page: cut through it, a page at a time.
            while (lines[at].top + lines[at].height - pageTop > height) {
                pageTop += height;
                breaks << pageTop;
            }
        }
    }
    return breaks << std::max(pageTop + 1, end);
}

} // namespace PageLayout
