#pragma once

#include <QList>
#include <QString>
#include <QtTypes>

#include <expected>
#include <span>
#include <vector>

#include "bookplace.h"
#include "inkstroke.h"

class Library;

// Book ink (#83): handwriting and drawing directly on a page of a book whose
// pages don't reflow, a PDF or a comic. A page's ink is an annotation of the
// kind "ink" on that page, made with its first stroke and gone with its last,
// so it is listed, synced and removed as every annotation is; its strokes are
// a set of that annotation in the change log ("set:strokes", ADR 0002's sets:
// one operation an ink batch, of each stroke the latest word holding, so what
// two computers drew is all kept). They are never rows of their own.
//
// A stroke's points are in ink units, `pageUnits` across the page's width
// and as many down (PageRegion's measure, times this), so ink stays where it
// was put at every zoom, in every layout and on any screen. The page is the
// place: a page has no words to find it again by in another edition, so the
// annotation keeps the hash of the file it was drawn on, and nothing more is
// guessed. Two computers that start ink on the same page of the same book make
// the same annotation (its global id is its book, kind and page), so their
// strokes meet on it.
namespace BookInk {

inline constexpr auto kind = "ink";
inline constexpr double pageUnits = 1000.0;

// A page's ink: its annotation's number and its strokes, in the order they began.
struct Page {
    qint64 id = 0;
    int page = 0;
    std::vector<Ink::Stroke> strokes;
};

// A book's pages with ink, in the order of the book.
[[nodiscard]] QList<Page> pages(const Library &library, qint64 bookId);
// The strokes on one page of a book.
[[nodiscard]] std::vector<Ink::Stroke> strokes(const Library &library, qint64 bookId, int page);

// What an ink surface did on a page (an ink batch, or an undo of one), kept
// in the change log in one write: the page's annotation made when it has none
// (placed in the file of `fileHash`), and removed when no stroke is left.
// Why not, when it can't be written.
std::expected<void, QString> write(Library &library, qint64 bookId, int page, const Ink::Change &change, const QString &fileHash = {});

// The words a page's handwriting was recognised as (#84), kept as the text of
// its annotation, which is what a search finds it by (#115, AnnotationWords)
// and which travels with it. Whether the page has ink to keep them on.
bool setWords(Library &library, qint64 bookId, int page, const QString &words);

// Strokes one over the other as they began (a tie by id, the same on every computer).
void inOrder(std::vector<Ink::Stroke> &strokes);

// The part of the page the strokes cover, with a margin of `margin` ink units.
[[nodiscard]] PageRegion regionOf(int page, std::span<const Ink::Stroke> strokes, double margin = 0);

} // namespace BookInk
