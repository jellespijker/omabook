#pragma once

#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <QStringView>
#include <compare>
#include <optional>
#include <utility>

class BookText;
struct FoundPassage;

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
// after the layout or the book's file changed (#80). A passage lies within
// one section, as a selection on a chapter or a page does; its places count
// characters of that section's text (BookText), a fraction of it.
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

    // The passage from character `start` to `end` of a section's text, with a
    // few words before and after it. Of the book's own text (BookText::text)
    // its places are exact; of a text that shows the section otherwise (the
    // reader's page) its words are right and its places near, and findAgain
    // then places it in the book's text.
    [[nodiscard]] static Passage of(QStringView sectionText, int section, qsizetype start, qsizetype end);
    // The passage in a book's text, as it is now: by its words first, the
    // words around them and its place telling one occurrence from another,
    // nearest its own section first; then by most of its words, when some
    // changed (a typo fixed, a word put in). Its words and places are
    // the text's own then. None when too little of it is there: an
    // annotation on it is lost.
    [[nodiscard]] std::optional<FoundPassage> findAgain(const BookText &text) const;
    // Where it is in a text showing its section (the reader's page, the
    // plain text of a laid-out chapter): its first character and the one past its last.
    [[nodiscard]] std::optional<std::pair<qsizetype, qsizetype>> spanIn(QStringView shown) const;
};

// A part of a page of a book whose pages don't reflow (a PDF, a comic): the
// page (its section) and a rectangle on it, measured across and down in the
// page's width, so that it is the same part of the page at any zoom, and is
// known before the page's height is (a comic's page is measured once drawn).
// Book ink is kept on its page in these terms (#83, BookInk), and so is a
// highlight or a margin note on a comic's panel, which has no words to find
// it by (#115, Annotation::area).
struct PageRegion {
    int page = 0;
    QRectF area; // 0 to 1 across; down, 0 to the page's height over its width

    bool operator==(const PageRegion &) const = default;

    // Where it starts as a place, on a page `aspect` (its height over its width) tall.
    [[nodiscard]] BookPlace place(double aspect) const;
    // {"page", "x", "y", "width", "height"}, as the plugins and the command line write it.
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PageRegion fromJson(const QJsonObject &object);
    // A rectangle as the annotations table and the change log keep it, "x y
    // width height" in the shortest numbers that read back the same; '' for
    // none (an empty one). And read back: an empty one for anything else.
    [[nodiscard]] static QString areaText(const QRectF &area);
    [[nodiscard]] static QRectF areaOf(QStringView text);
};

// A passage found again, and how sure that is, from 0 to 1: 1 when its words
// and those around them are all there, less as fewer are.
struct FoundPassage {
    Passage passage;
    double confidence = 0;
};
