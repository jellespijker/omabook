# Omabook

An ebook library and reader whose books, reading and knowledge stay one's own.

## Annotations

**Annotation**:
Anything a reader adds to a book at a place in it: a highlight, a margin note or book ink.
_Avoid_: Note (alone), markup, comment

**Highlight**:
A passage of a book marked in a colour.
_Avoid_: Underline, mark, selection

**Margin note**:
Words about a passage or place in a book, typed or written by hand, shown beside it.
_Avoid_: Note (alone), comment, annotation (for this kind only)

**Margin column**:
The strip beside the text where margin notes stand, each at the height of its passage; part of the page's layout, there only when the window has room for it beside a comfortable page (`PageLayout::columns`).
_Avoid_: Sidebar, notes pane, gutter (the space between pages)

**Book ink**:
Handwriting or drawing directly on a page of a book whose pages don't reflow, such as a PDF or a comic.
_Avoid_: Drawing, scribble, ink (alone, which is the notebook plugin's)

**Ink surface**:
Where a pen writes and draws, in Omabook or a plugin's panel: pressure makes the line wider, the pen's other end erases, a palm is ignored while the pen is near (`InkSurface`).
_Avoid_: Canvas, drawing area

**Stroke**:
One line of ink, from the pen touching down to lifting: its points, each with its pressure, tilt and moment, kept as data rather than a picture (`Ink::Stroke`).
_Avoid_: Path, line (alone), scribble

**Ink batch**:
What an ink surface did between two pauses of the pen, strokes added and removed, kept as one change wherever its strokes are kept; erasing part of a stroke is its removal and its pieces' addition (`Ink::Change`).
_Avoid_: Diff, delta, update

**Lost annotation**:
An annotation whose passage can no longer be found in its book, kept until the reader places or removes it.
_Avoid_: Orphan, broken annotation

**Annotation layer**:
Someone else's annotations on a book, shown beside one's own and cut at one's own place.
_Avoid_: Shared highlights, overlay

**Place**:
A point in a book in the book's own terms: its section (a chapter, or a page of a PDF or comic) and how far into it, so it is the same text in any window. Where a book was read to, where the graph met a thing, and where a bookmark is are all places (`BookPlace`).
_Avoid_: Position (alone), location, offset

**Passage**:
A stretch of a book's text that an annotation is attached to: two places and the words between, with a few before and after, found again by its words when the book's layout changes (`Passage`).
_Avoid_: Selection, range, quote, excerpt (the text the graph is built from)

## Collections and the graph

**Tenant**:
A top-level collection's own part of the knowledge graph, kept entirely apart from every other: nothing in it is shared with, merged into or read from another tenant.
_Avoid_: Side, book side, partition, scope (for this meaning)

**Tenant membership**:
Which tenants see a thing, a fact or a relation of the graph, each stored once: those of the books it was learned from.
_Avoid_: Copy, partition, side

**Unfiled**:
The tenant of the books that are in no top-level collection.
_Avoid_: None, default, uncategorised
