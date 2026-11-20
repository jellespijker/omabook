# 6. Book ink is a set of strokes on its page's annotation

- Status: accepted (#83)
- Date: 2026-11-20
- Builds on: [ADR 0002](0002-plugin-data-in-the-change-log.md) (sets in the change log), [ADR 0004](0004-passages-found-by-their-words.md)

## Context

Book ink is handwriting on a page of a PDF or a comic. It has to stay where it was put at every zoom
and layout, travel between one's own computers without losing what either drew, and be listed, undone
and removed like the other annotations. A page has no words to find a passage by (ADR 0004), and a
page of handwriting is thousands of strokes that change a few at a time.

## Decision

- A page's ink is **an annotation of the kind "ink"** on that page (its section), made with its first
  stroke and removed with its last. It is listed, synced and tombstoned as every annotation is.
- Its strokes are **a set of that annotation in the change log** (`set:strokes`, as a plugin's sets are
  in ADR 0002): one operation is an ink batch (`{"add": {id: stroke}, "remove": [id]}`, `Ink::batches`),
  of each stroke the latest word holds, so what two computers drew is all kept and an erasing on either
  goes to both. They are never rows.
- The annotation's **global id is its book, kind and page**, so two computers that start ink on the same
  page make the same annotation and their strokes meet; made again after its last stroke was erased, it
  is the one it was.
- Points are in **ink units across the page's width** (1000, `BookInk::pageUnits`, and as many down), the
  page's own measure, so the same strokes are drawn at every zoom and screen. The part of a page ink
  covers is a **page region** (`PageRegion`), in the same measure; a highlight or note on a comic's
  panel is on one too (#115, the annotation's `area`), its page its place as ink's is.
- The page is the place. The annotation keeps the hash of the file it was drawn on; nothing more is
  guessed when the book's file changes.

## Considered options

- **A table of strokes**, replayed from the log: a migration, and a replay of sets the log already does.
- **One operation per stroke**: thousands of operations for a page, against one per pause of the pen.
- **A snapshot of the page per write**, the latest winning: one computer's strokes lost when both drew.

## Consequences

- An Omabook from before takes the annotation and leaves its strokes in the log, where they are read
  once it is updated; it shows no ink.
- Ink is found by search through handwriting recognition (#84, #115): where a model is chosen to read
  it, a page is read once the pen rests, only by one's own servers, and its words are written into the
  annotation's text, which the annotations' word index holds (`BookInkWords`, `AnnotationWords`).
- Undo goes a batch at a time over every page (`ReadingInk`), not a stroke at a time.
