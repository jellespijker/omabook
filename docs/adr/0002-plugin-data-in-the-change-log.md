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

## As built (#76)

- **A space per plugin.** A plugin writes `plugin:<id>` (`PluginData`), its entities named `<id>:<entity>` in
  it, so no plugin's field is taken for the library's or another plugin's where compaction keeps the latest
  of each (it matches a field by device, entity, thing and field, not by space). A plugin reads and writes
  its own space alone (`omabook.log.*`, `ScriptChangeLog`).
- **Out of the rows.** What the rows are made of (`live_ops`) leaves `plugin:` spaces out (format 33), as it
  does published and shared ones; `space_ops` holds them for replication, compaction and `PluginData`.
- **Three kinds of operation.** A field (its JSON value, the latest winning); a change of a set
  (`set:<name>`, `{"add": {key: value}, "remove": [key]}`), so one operation carries a batch, or an
  erasing in part, and of each item the latest word holds, an addition winning a removal at the same
  moment; and a removal (`removed`), which a later write undoes. Compaction keeps every change of a set
  (`SpaceReducer::grows`), so what two computers added survives it.
- **Between one's own computers only.** The ssh replication carries every `plugin:` space; a sync says it
  takes plugin data (`"plugin_data": 1`), and a side that doesn't (an Omabook from before, whose log
  would leave it out and whose vector would then never hold it) is sent none. No space carries plugin data
  yet.
- **Limits on what arrives.** A value of more than 64 KiB, a change of more than 1000 items, a name or id
  out of bounds, another plugin's entity, or what is no field or change of a set is refused at a write
  (thrown into the script) and when it comes from elsewhere (`PluginData::whyRefused`, counted with the
  refused in a sync's summary), from one's own computers too.
- **Still open.** A part of a space for a plugin's data (the parts registry), so a space takes it only when
  switched on; reading it cut at a place in a book; and an event that tells a plugin its data changed at a
  sync.

## As built (#85)

- **The parts registry.** What a space holds beyond its collection and its books is one table, `SpaceParts`
  (graph, questions, ratings, reading, files, annotations), which publishing, founding, switching, the command
  line and the MCP server read. A part the registry says is *switched* is switched on and off after a shared
  space's founding by an operation of the space (entity `space`, field `part:<name>`, the latest by clock
  winning, by the role rules), and in a space published here by its owner, whose log then follows. A plugin's
  part is still to come.
- **The first switched part is a person's annotations** (highlights, margin notes, book ink, `SpaceAnnotations`),
  off until switched on. Others' annotations are read as a layer, never as rows (`AnnotationLayers`), and
  what another person sent is held to this ADR's limits: an operation of more than 64 KiB, or a change of a set of
  more than 1000 items, is left out and counted; of an annotation no more than 1000 strokes are drawn; its words
  are plain text, without controls or the characters that turn text around.
