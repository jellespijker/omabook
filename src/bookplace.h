#pragma once

#include <QJsonObject>
#include <QString>
#include <compare>

// A place in a book, whatever its format: the section it is in (a chapter of
// a flowing book, EPUB, FictionBook, Markdown or text; a page of a PDF or a
// comic, as Document counts them) and how far into that section, from 0 at
// its start to 1 at its end. It says the place in the book's own terms, so it
// is the same text on a window of any size and in any font.
//
// Every place in a book is one of these: where it was read to (Progress),
// where the graph met a thing or learned a fact (GraphPlace), the place of a
// bookmark or highlight (Annotation), and the two ends of a Passage. Places
// are ordered as the book is read: by section, then by how far into it.
struct BookPlace {
    int section = 0;
    double fraction = 0;

    constexpr auto operator<=>(const BookPlace &) const = default;
    constexpr bool operator==(const BookPlace &) const = default;

    // As every interface writes a place, the plugins' and the command line's:
    // {"section": 2, "fraction": 0.375}.
    [[nodiscard]] QJsonObject toJson() const;
    // The same two keys added to an object that says more of the place.
    [[nodiscard]] QJsonObject toJson(QJsonObject alongside) const;
    // The place an object of that shape says; a key it leaves out is
    // `whenMissing`'s, or 0.
    [[nodiscard]] static BookPlace fromJson(const QJsonObject &object);
    [[nodiscard]] static BookPlace fromJson(const QJsonObject &object, const BookPlace &whenMissing);
};

// A stretch of a book's text that an annotation is attached to: where it
// starts and where it ends, and the words between, with a few just before and
// after them (as the W3C web annotation's text quote selector keeps them). The
// places find it quickly; the words find it again when they no longer do,
// after the layout or the book's file changed.
struct Passage {
    BookPlace from;
    BookPlace to;
    QString quote;  // the words of the passage, as the text has them
    QString before; // a few words just before it, to tell one occurrence from another
    QString after;  // and a few just after

    bool operator==(const Passage &) const = default;

    // Whether a place lies within the passage, its ends included.
    [[nodiscard]] constexpr bool contains(const BookPlace &place) const noexcept { return from <= place && place <= to; }

    // {"from": {place}, "to": {place}, "quote", "before", "after"}, the words
    // left out when there are none.
    [[nodiscard]] QJsonObject toJson() const;
    // The same two keys added to an object that says more of the place.
    [[nodiscard]] QJsonObject toJson(QJsonObject alongside) const;
    [[nodiscard]] static Passage fromJson(const QJsonObject &object);
};
