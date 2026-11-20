#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringView>
#include <QVector>

// The colours a highlight can be (#81): five, each known by a name of its
// own ("yellow"), which is what the library, the change log and the plugins
// keep, and what the reader calls it ("important"), which is theirs to choose.
// They are the Okabe-Ito colours, told apart by readers who see colours
// differently too, and each has a line of its own under the words (solid,
// dashed, dotted...) for when the colour alone doesn't say which it is.
//
// On the page a highlight is its colour laid thinly over the background: as
// thinly as it takes for the text over it to stay as readable as the page's
// (`tint`), whatever the theme, light or dark; its line is the colour made
// lighter or darker until it stands out from the background (`line`).
namespace HighlightColors {

struct Color {
    QString key;
    QColor color;
    // The line under the words: dashes and gaps, in widths of the line; none for a solid one.
    QVector<qreal> dashes;
};

// The five, in the order they are offered; the first is a highlight's when none is said.
[[nodiscard]] const QList<Color> &all();
// The colour of that name; the first for '' or a name it doesn't know.
[[nodiscard]] const Color &of(QStringView key);
[[nodiscard]] bool known(QStringView key);
// As the library keeps it: '' for the first, the name for the others; '' for one it doesn't know.
[[nodiscard]] QString stored(QStringView key);

// WCAG's contrast of two colours, from 1 to 21.
[[nodiscard]] double contrast(const QColor &one, const QColor &other);
// The colour laid over `background` as strongly as leaves `text` over it at
// the contrast of normal text (4.5), or as near to that as it gets.
[[nodiscard]] QColor tint(QStringView key, const QColor &background, const QColor &text);
// Its line under the words: the colour, lighter or darker until it stands
// out from `background` as a mark should (3).
[[nodiscard]] QColor line(QStringView key, const QColor &background);

} // namespace HighlightColors
