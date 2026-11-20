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

**Book ink**:
Handwriting or drawing directly on a page of a book whose pages don't reflow, such as a PDF or a comic.
_Avoid_: Drawing, scribble, ink (alone, which is the notebook plugin's)

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
