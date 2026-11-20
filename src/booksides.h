#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <ranges>

#include "library.h"

// Which books the knowledge graph links to one another. A library split into
// collections at the top (one for study, one for leisure, as its owner names
// them) keeps those parts apart: a book's side is the set of top collections
// it is in, through any collection inside them, and two books are linked
// only when their sides meet. A book in no collection is on a side of its
// own with the other books in none. A book in two top collections is on
// both sides: its owner put it in both, so it links with either, and what
// a book of one side and a book of the other share is still one thing.
//
// Nothing is removed by this: a link between books on two sides stays where
// it is kept, unseen and unused, and comes back when a book moves to the
// other's side or the setting that keeps sides apart is turned off.
//
// A library without collections, or with that setting off, is one side, and
// every book is linked as it always was.
class BookSides {
public:
    // A side: the top collections a book is in, by number, or `none` alone
    // for a book in no collection. An empty side is no side at all, which
    // meets every side: a book the library doesn't know, or a thing of no
    // book.
    using Side = QSet<qint64>;
    static constexpr qint64 none = 0;

    // Everything on one side.
    BookSides() = default;
    // The sides of these books, in this tree of collections (Library::collections).
    BookSides(const QList<Collection> &collections, const QList<Book> &books);
    // The library's sides as its collections are now, and as the user chose
    // (Settings::graphOneTenant): one side when the graph is one tenant.
    // Read once and kept until the books, the collections or the choice change.
    [[nodiscard]] static BookSides of(const Library &library);

    // Whether there are sides to keep apart at all.
    [[nodiscard]] bool apart() const noexcept { return !m_sides.isEmpty(); }
    [[nodiscard]] Side side(qint64 bookId) const { return m_sides.value(bookId); }
    // The sides of several books together: what a thing met in all of them is on.
    template <std::ranges::input_range Ids>
        requires std::convertible_to<std::ranges::range_value_t<Ids>, qint64>
    [[nodiscard]] Side sideOf(const Ids &books) const {
        Side all;
        for (const qint64 book : books)
            all += side(book);
        return all;
    }
    // Whether two sides meet: they share a top collection, or one is no side.
    [[nodiscard]] static bool meet(const Side &a, const Side &b) { return a.isEmpty() || b.isEmpty() || a.intersects(b); }
    [[nodiscard]] bool together(qint64 a, qint64 b) const { return !apart() || meet(side(a), side(b)); }

private:
    QHash<qint64, Side> m_sides;
};
