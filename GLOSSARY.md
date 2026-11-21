# Omabook

An ebook library and reader whose books, reading and knowledge stay one's own.

## Annotations

**Annotation**:
Anything a reader adds to a book at a place in it: a highlight, a margin note or book ink.
_Avoid_: Note (alone), markup, comment

**Highlight**:
A passage of a book marked in a colour.
_Avoid_: Underline, mark, selection

**Highlight colour**:
One of the five colours a highlight is in, known by its own name (yellow, blue, green, orange, purple) and called what the reader chooses ("Important"); each has a line of its own under the words, so it is told apart without its colour (`HighlightColors`).
_Avoid_: Category, tag, label

**Pen gesture**:
A stroke of the pen over a book's text that means something rather than staying as ink: a line under words, a loop round them or a bar beside lines highlights them; a scribble takes highlights away (`Ink::gestureOf`).
_Avoid_: Ink (for this), drawing, shortcut

**Margin note**:
Words about a passage or place in a book, typed or written by hand, shown beside it.
_Avoid_: Note (alone), comment, annotation (for this kind only)

**Handwritten margin note**:
A margin note written with the pen: its ink is the note, kept as strokes on it; its words are what was read of the ink, searched and shown on request (`NoteRecognition`).
_Avoid_: Ink note, scribble, drawing

**Margin column**:
The strip beside the text where margin notes stand, each at the height of its passage; part of the page's layout, there only when the window has room for it beside a comfortable page (`PageLayout::columns`).
_Avoid_: Sidebar, notes pane, gutter (the space between pages)

**Book ink**:
Handwriting or drawing directly on a page of a book whose pages don't reflow, such as a PDF or a comic.
_Avoid_: Drawing, scribble, ink (alone, which is the notebook plugin's)

**Page region**:
A part of a page of a book whose pages don't reflow, measured across and down in the page's width, so it is the same part at any zoom: where book ink lies on its page, and what a highlight or a margin note on a comic's panel is on, which has no words to find it by (`PageRegion`, `Annotation::area`).
_Avoid_: Area, box, rectangle, selection

**Ink surface**:
Where a pen writes and draws, in Omabook or a plugin's panel: pressure makes the line wider, the pen's other end erases, a palm is ignored while the pen is near (`InkSurface`).
_Avoid_: Canvas, drawing area

**Stroke**:
One line of ink, from the pen touching down to lifting: its points, each with its pressure, tilt and moment, kept as data rather than a picture (`Ink::Stroke`).
_Avoid_: Path, line (alone), scribble

**Ink batch**:
What an ink surface did between two pauses of the pen, strokes added and removed, kept as one change wherever its strokes are kept; erasing part of a stroke is its removal and its pieces' addition (`Ink::Change`).
_Avoid_: Diff, delta, update

**Recognised text**:
The words of handwriting as a vision model read them, kept with the strokes they came from by the hash of the ink and the model, so it is read once and found by search on every computer (`RecognitionStore`).
_Avoid_: OCR (alone), transcription (for ink)

**Transcript**:
The words of a recording, each with its moment, written out by a transcription server or by whisper.cpp on this computer (`Recognition::Result`).
_Avoid_: Captions, subtitles

**Own server**:
A server what is written and said may be sent to without asking: on this computer, on the tailnet, or at an address the person marked as theirs. Any other is hosted, and is sent it only where chosen twice over: allowed in the settings and asked for (`RecognitionRoute::whyNot`).
_Avoid_: Local server (for the tailnet), trusted server

**Lost annotation**:
An annotation whose passage can no longer be found in its book, kept until the reader places or removes it.
_Avoid_: Orphan, broken annotation

**Annotation layer**:
Someone else's annotations on a book, shown beside one's own, under the name they chose (`MemberNames`) and in their colour, and cut at one's own place: carried by a space whose annotations part is on, switched off one person at a time or all (`AnnotationLayers`).
_Avoid_: Shared highlights, overlay

**Part of a space**:
What a space holds beyond its collection and its books, switched on by name: its graph, questions, ratings, reading, files and annotations. What is one's own is off until one switches it on (`SpaceParts`).
_Avoid_: Section, feature, scope (for this meaning)

**Private book**:
A book of a kind its plugin keeps the person's own (a notebook, `"private"` in its plugin.json): in their own collections and on their own computers, and in no shared or published space unless they let that one book go (`PrivateBooks`).
_Avoid_: Hidden book, secret book, personal book

**Place**:
A point in a book in the book's own terms: its section (a chapter, or a page of a PDF or comic) and how far into it, so it is the same text in any window. Where a book was read to, where the graph met a thing, and where a bookmark is are all places (`BookPlace`).
_Avoid_: Position (alone), location, offset

**Passage**:
A stretch of a book's text that an annotation is attached to: two places and the words between, with a few before and after, found again by its words when the book's layout changes (`Passage`).
_Avoid_: Selection, range, quote, excerpt (the text the graph is built from)

**Stretch**:
A run of about a hundred and sixty words of a book's text, sharing its first and last forty with the stretches beside it, within one section: what the meaning index keeps and a search by meaning finds (`TextStretches`).
_Avoid_: Chunk, passage (an annotation's), excerpt (the graph's)

**Meaning index**:
A book's stretches, each with the embedding a model on the person's own server gave it, kept in the library beside the book and never in the change log; made only for the books asked for (or every book, by a setting) and made again when the book's file changes (`MeaningIndex`).
_Avoid_: Vector store, semantic index, embeddings (for the whole)

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
