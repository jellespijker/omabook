# Asking about books, and the knowledge graph

Part of the Omabook skill (`omabook skill graph`).

## Ask about a book

```bash
omabook ask 12 "Who is the narrator?"            # the configured AI answers from the book's text
omabook ask 12 --upto 5 "Summarize so far"      # only sections 0 to 5 are sent, so nothing later is spoiled
```

When you can read the text yourself with `text` and `find`, prefer that to `ask`. Omabook keeps two models: `main` for answers and writing, `quick` (a small, fast one) for reading text into structure, such as the knowledge graph; any command that asks the AI takes `--model main|quick|<name>` for that run, and the MCP tools that ask take `model`. The AI that answers is the one Omabook is set to: by default Omarchy's default agent (`omarchy default agent`), run for one answer with its tools off; `omabook info --json` says which.

## The knowledge graph

Omabook keeps a graph of what was read: characters, places, groups, concepts, items, events, plot threads and themes of each book, the real-world topics it touches, the questions that were asked, and the books, authors, series and tags, with where in which book each fact was learned. It is never ahead of a book's reader: do not tell the user what lies past their reading place.

```bash
omabook graph show 12 --json                 # a book's graph: {"nodes": [...], "edges": [...]}; --upto 5 cuts it at a section
omabook graph show --json                    # the whole general graph, the most connected things first; --scope world:42 for another
omabook graph find tide --kind concept --json   # search names, other names and summaries
omabook graph node 31 --depth 2 --json       # one node, where it was met, and what is around it
omabook graph add --kind character --name "Mara Venn" --summary "Keeps the lighthouse." --alias Mara --book 12 --section 3
omabook graph link 31 44 "mentor of" --note "since the first winter"   # unlink <edge>, merge <keep> <other>, remove <id>
omabook graph build 12                       # have the configured AI grow it, as far as the book was read; costs AI calls
omabook graph export 12 --format graphml --output book.graphml   # json, graphml, dot, cypher
omabook graph scopes --json                  # copy-scope <from> <to> forks a graph; remove-scope <scope> throws one away
omabook graph connect 12 --json              # have the AI link the book's themes, ideas and threads to the other books read; --estimate sends nothing
omabook graph show 12 --beyond --json        # with "beyond": the things of other books it connects to, and the books that share a thing with it
omabook graph log --json                     # the reading log: per day, from what percent to what percent
```

A relation whose kind starts with `across: ` (`across: echoes`, `"across": true` in JSON) is a connection between things of two books, noticed while reading, not a fact either book states.

Kinds: `character`, `place`, `group`, `concept`, `item`, `event`, `thread`, `theme`, `topic`, `question`, `book`, `author`, `series`, `tag`. Adding a node of a name that exists (in the same scope, of the same kind) brings that node up to date. Write what you make up into a scope of its own (`--scope world:<name>`), not into the general graph, which is what the books say.
