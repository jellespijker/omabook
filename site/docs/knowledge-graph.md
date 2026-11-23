<!-- The knowledge graph: https://jellespijker.github.io/omabook/docs/knowledge-graph.html -->

# The knowledge graph

Omabook keeps a graph of what you read, in the library’s own database: the characters, places, groups, concepts, items, events, plot threads and themes of each book; the topics beyond the book that it touches; and the books, authors, series and tags themselves.

[Picture: The graph of a library: books, authors and tags on the left, the people, places and ideas of a story on the right.]

Press 6 for the library as a graph. (demo library of made-up books)

## It grows as you read

When you finish a chapter, the model is sent the pages read since last time, with the names of what the graph already holds, and answers with things and relations. A marker with the book says how far that got, so no page is sent twice. It costs AI calls, so it is a setting: *grows as you read* (the default), *only when you ask for it* (`omabook graph build <book>`) or *off*.

## Never past your page

Anything past the furthest you have read in a book is a spoiler. It is not shown, searched or sent to the AI. The details keep a count, *Later in the book (n)*, and opening it asks first.

[Picture: A novel with its characters marked and the map of the book so far beside the page.]

Press M while reading for the map so far (demo library of made-up books).

## Across books

A character of one series is never merged with one of the same name in another, but a theme or a real-world topic is one node shared by every book that touches it. That is what links your reading together. Once per build of a book, a small request puts its most telling things beside those of other books you read, and notes what connects them: echoes, contrasts with, and the like. They show as dashed lines.

## Timelines

The chronology puts events in the order they *happen*, not the order you read them. `T` on the shelf shows the selected book’s chronology; `Shift+T` while reading shows the open book’s. One lane per timeline on a ruler: the book’s own, its series’, a world several books share, or real history. A real date puts an event on real history, which is how books with nothing else in common meet. Events with no time are listed apart, and contradictions are ringed with where each side was said.

## By hand and by command

`N` adds a thing, `E` edits it, `Shift+M` folds two into one. What you put in is marked as yours and wins. Export with `omabook graph export 12 --format graphml` (also json, dot, cypher).

```
omabook graph find tide --kind concept
omabook graph node 31 --timeline --book 12 --upto 5
omabook chronology --collection Novels --timeline "Real history" --from 1940 --to 1942
```
