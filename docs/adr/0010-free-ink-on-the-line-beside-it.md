# 10. Free ink on a flowing book is anchored to the line beside it

- Status: accepted (#152)
- Date: 2026-11-22
- Builds on: [ADR 0004](0004-passages-found-by-their-words.md) (passages), [ADR 0006](0006-book-ink-on-its-page.md) (strokes as a set of an annotation)

## Context

A reader writes in the margins of a paper book and between its lines: a bar beside a paragraph, a word
above a line, an underline, an arrow. A flowing book (EPUB, FictionBook, Markdown, text) has no page for
that ink to stay on (ADR 0006): its lines are broken anew for every size and face of type, window width
and layout (a column that scrolls, pages that turn), and a new edition moves its words. Ink has to stay
beside what it was written beside, at a size that reads beside that text, travel between one's own
computers and into spaces, and be listed, undone and removed as other annotations are. A handwritten
margin note (#90) is on words selected first, on its card; this ink is anywhere, on nothing chosen.

## Decision

- **Each stroke is anchored to the line beside it**: the line its middle is on or, between two lines,
  the nearer, a line reaching down a third of its height further so that an underline is its line's
  (`InkLines::anchorOf`).
- **A line's strokes are an annotation on a passage of that line** (ADR 0004), so they are found again by
  their words in a new edition, and wherever the text is laid out:
  - in a side margin (the stroke's middle past an edge of the column), the kind **"margin-ink"**, on the
    whole line; a stroke is measured from that edge, outward, and the line's top, so it stays in the margin
    however the line is broken, left or right as it was drawn;
  - among the lines, the kind **"line-ink"**, on the words under or over it (at least one word); measured
    from the first of them, so it goes where they go: under the word it underlines.
  New strokes beside a line join its annotation (the same line in a margin, some of the same words among
  the lines); a stroke moved by the lasso or cut by the eraser is anchored again where it now is.
- **Strokes are measured in the line's height** (`InkLines::lineUnits`, 100 to a line), so larger type
  draws them larger: ink stays as readable beside its text as it was.
- Their strokes are **a set of the annotation in the change log** (`set:strokes`, AnnotationInk), as a
  page's book ink is; the annotation goes with its last stroke. It is synced, listed (by chapter), carried
  into spaces and exported as the other annotations are.
- **A margin narrower than its ink** (large type, a narrow window, #157: the margin column for notes may
  not fit at all) draws that side's ink smaller, as much as it takes to stay within the page's own
  margin, and at its size again once there is room. Nothing is moved over the text, and nothing is hidden.
- The inks are kept as the inks they are, and drawn to read on the theme's page: the first in the text's
  own colour, the others lighter on a dark page.

## Considered options

- **One annotation per chapter**, strokes in the chapter's pixels (as a page's book ink): wrong at the
  first change of type or width.
- **Points as fractions of the column's width**: a margin's ink would move into the text as the column
  narrows, and handwriting would be stretched sideways.
- **One kind, with the side in a new field of the annotation**: a schema migration for what the two kinds
  already say; ink in a margin and ink among the lines are found and placed differently, which is what a
  kind is.
- **All of a pause of the pen on one line** (a note written down several lines kept whole): it would keep
  a handwritten word together, but lines broken anew put text between the lines it spans; a stroke on the
  line it is beside is what a reader expects to follow the text.

## Consequences

- An Omabook from before takes the annotations and leaves their strokes in the log, and shows no ink.
- A word written across two lines is two lines' ink, and drawn apart when they are.
- Ink beside a line whose words are gone in a new edition is lost with it: kept and not drawn, and drawn
  again in an edition that has them.
- Ink on a flowing book is not read by handwriting recognition yet; a handwritten margin note is.
