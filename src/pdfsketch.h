#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <expected>
#include <span>

// A PDF drawn from a description (#138): what `omabook.pdf` lets a plugin
// write, a notebook's pages, say, without carrying a PDF writer of its own.
// It is drawn with Qt's own (QPdfWriter): paths stay vectors, text is real
// text in a font that is embedded, and pictures are decoded here, whatever
// format Qt reads.
//
// The description, in points (1/72 inch) from the top left of each page:
//   {title, pages: [{width, height, items: [...]}]}
// and each item one of
//   {type: "path", points: [x0, y0, x1, y1, ...], width, color, alpha, fill, closed}
//   {type: "text", text, x, y, size, font, color, alpha, bold, italic, width}
//   {type: "image", image, x, y, width, height}
// A colour is what QColor reads ("#3366cc", "red"), black when it reads
// nothing; `alpha` (0 to 1) is laid over its own. A path is stroked `width`
// wide with round ends, and filled with `fill` when there is one. Text stands
// with its top at `y`, wrapped at `width` when it is given. `font` is ""
// (the app's), "reader" (the reader's), "mono", or a family by name. An
// image is an index into the pictures handed over beside the description.
namespace PdfSketch {

// The families "", "reader" and "mono" stand for.
struct Fonts {
    QString app;
    QString reader;
    QString mono;
};

// The most it draws, so a script can't have it write without end.
constexpr int maxPages = 2000;
constexpr int maxItems = 200000;
// A side of a page, in points: 200 inches, the most PDF readers take.
constexpr double maxSide = 14400;

// Why nothing was drawn.
struct Problem {
    enum class Kind {
        NoPages,      // no page at all
        TooManyPages, // over maxPages
        TooManyItems, // over maxItems, on all pages together
        PageSize,     // `index`, a page, is no size or larger than maxSide
        Picture,      // `index`, an item's image, is none of those given or no picture Qt reads
        Unwritable,   // Qt's writer gave nothing
    };
    Kind kind = Kind::Unwritable;
    int page = 0;  // counting from 0
    int index = 0; // of the page, or of the item on it
};

[[nodiscard]] std::expected<QByteArray, Problem> render(const QJsonObject &sketch, std::span<const QByteArray> pictures,
                                                        const Fonts &fonts);

} // namespace PdfSketch
