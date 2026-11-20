#pragma once

#include <QList>
#include <QString>
#include <QStringView>
#include <expected>
#include <optional>

#include "library.h"

class BookText;

// A book's highlights (#81): passages marked in one of the named colours
// (HighlightColors), made, coloured anew and listed the same way by the
// window and the command line. They are annotations of the kind "highlight",
// kept and found again as every passage is (#80, AnnotationAnchors), and
// travel by the change log with their colour.
namespace Highlights {

inline constexpr auto kind = "highlight";

// A book's highlights in the order of the book; with a colour, only those of it.
[[nodiscard]] QList<Annotation> of(const Library &library, qint64 bookId, QStringView color = {});
// One, by its number, whatever its book: the book's number and it.
[[nodiscard]] std::optional<std::pair<qint64, Annotation>> find(const Library &library, qint64 id);

// A new highlight on a passage, in a colour. A passage taken from a page
// that shows its section otherwise (the reader's) is placed in the book's
// own text when `text` is given, and so in the file of `fileHash`; without
// it, it is placed the next time the book is opened. Its number, or why not.
[[nodiscard]] std::expected<qint64, QString> make(Library &library, qint64 bookId, const Passage &passage, QStringView color,
                                                  const BookText *text = nullptr, const QString &fileHash = {});
// A new highlight on a part of a page, a comic's panel, which has no words
// to find it by (#115): the page `aspect` (its height over its width) tall,
// and drawn from the file of `fileHash`. Its number; none for a colour there
// isn't, a part beside the page, or when it can't be kept.
[[nodiscard]] std::optional<qint64> makeOn(Library &library, qint64 bookId, const PageRegion &region, double aspect, QStringView color,
                                           const QString &fileHash = {});
// Another colour for one; whether there is a highlight of that number.
bool recolor(Library &library, qint64 id, QStringView color);

} // namespace Highlights
