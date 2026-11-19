# 2. A plugin's data travels as operations in the change log

- Status: accepted, to be built with the notebooks' extension points (#76; jellespijker/omabook-notebooks#1)
- Date: 2026-11-20
- Builds on: [ADR 0001](0001-owned-spaces-and-replicated-logs.md)

## Context

Plugins keep data of their own. Today the only way to keep it with the library is a file: the plugin
writes it under its `data/` folder, or adds it to the library as a book in its own format and swaps in
new versions with `library.replaceFile` (as Storyteller does with a `.story`). Between two computers such
a file is replaced whole, so when both changed it, one side's changes are lost.

That is acceptable for a story, which one computer writes at a time. It is not for a notebook: a page is
written by hand on the laptop and on the desktop, and a stroke lost to "the last file wins" is lost
writing. A notebook's data is also many small things (strokes, text blocks) that change one at a time.

## Decision

A plugin may keep its data as **operations in the change log** of ADR 0001: entities and fields under
the plugin's id, written through the plugin API, signed by the person, replicated between one's own
computers, compacted, and carried into a space only when the space's part for it is switched on, like
the library's own personal parts.

- Fields merge by **latest wins**; a plugin can declare a field a **set** (strokes are only added or
  erased), so two computers' additions are both kept.
- Plugin data is kept out of the library's own rows, as published, subscribed and shared spaces are.
- The plugin's **file in the library is a snapshot** of that data, made for export, for opening without
  the plugin, and for an Omabook older than the extension point; it is never what travels.

## Considered options

- **The file, replaced whole** (today's way): simple, and enough for data one computer writes at a
  time; loses work when two computers write. Kept for plugins that want it.
- **A merge format inside the file** (a CRDT the plugin keeps itself): every plugin would carry its own
  replication, signing and sharing rules, apart from the ones the library already has and keeps safe.
- **Operations in the change log** (chosen): one replication, one signature, one privacy rule for
  everything that travels; the cost is an extension point in Omabook and a format plugins write through.

## Consequences

- The plugin API gains calls to write and read log operations (#76); their names and kinds become part
  of what a plugin may rely on across releases.
- Compaction, the older-peer path and the sharing switches have to treat plugin entities like the
  library's own; a plugin removed from a computer leaves its operations in the log, read again when it
  comes back.
