#pragma once

#include <QString>
#include <QtTypes>
#include <optional>

class Library;

// A link to a place in a book that opens Omabook there (#138):
// `omabook://book/<book>/section/<n>`, `<n>` counting from 0, or
// `omabook://book/<book>` for where it was left. `<book>` is the book's global
// id, the same on each of one's computers and in a space (Spaces::bookId), so
// a link written into an export works wherever the book is; a book's number
// in this library is taken too. The desktop entry hands such a link to
// `omabook open`, and a window already open takes it.
namespace BookLink {

struct Target {
    QString book;     // as the link names it
    int section = -1; // none: where it was left
};

// What a link points to; none when it is no link of this kind.
[[nodiscard]] std::optional<Target> parse(const QString &link);
// The link to a book by its global id, at a section when it is 0 or more.
[[nodiscard]] QString make(const QString &book, int section = -1);
// The book of the library a link names: by its global id, else by its number
// there; 0 when it has none of either.
[[nodiscard]] qint64 find(const Library &library, const QString &book);

} // namespace BookLink
