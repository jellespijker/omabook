# 4. Passages are found again by their words, in the file they were placed in

- Status: accepted (#80)
- Date: 2026-11-20
- Builds on: #88 (`BookPlace`, `Passage`)

## Context

A highlight or a margin note is on a passage of a book. Its place (a section and a fraction of it) is
quick to go to, but it is only right for the file it was taken from: a new edition, a typo fixed, a
paragraph put in, or reading another format of the book moves the text under it. The reader's layout
(font, window, hyphenation) never does, since places count characters of the book's own text
(`Document::text`), not of the page.

## Decision

- A passage keeps its **words** with a few before and after (as the W3C web annotation's text quote
  selector does) beside its **places** (its position selector), and the **hash of the file** its places
  were found in (`annotations.anchored`).
- When the book is read from a file it was not placed in, each passage is **found again by its words
  first**: the rarest of them point to where it may be, the words around it and its old place tell one
  occurrence from another, nearest its own section first. When some words changed, most of them in order
  still find it. Its words and places become the new file's, with a **confidence** (0 to 1).
- One that can't be found is a **lost annotation**: kept with its words, sought again in the next file,
  listed for the reader to place again or remove. Nothing is guessed from the place alone.
- It happens when a book is opened (beside the window) and when its annotations are listed, never as a
  sweep of the library, and only for passages not yet placed in that file: one question to the library.
- Passages travel in the change log as fields of the annotation, the latest winning, so a computer that
  found one again in its edition sends the result; a computer with another file finds it again in its own
  the next time it opens the book.

## Considered options

- **Places only**: what highlights were before; wrong after any change to the file.
- **Character offsets checked by a hash of the section**: tells that the text changed, not where the
  passage went.
- **Fuzzy matching of characters** (diff-match-patch): finds small edits too, but is slow on long sections
  and sensitive to reflow and spacing; matching words is enough for editions and typo fixes, and fast for
  a thousand highlights.

## Consequences

- Two computers with different editions of a book each find its passages again in their own when they
  open it, and the last to do so wins in the log until the other opens it again. Both are right for their
  own file.
- A passage lies within one section (a selection on a chapter or a page).
- Books read by converting them first (Kindle files) are not found again yet: their places are the
  conversion's.
