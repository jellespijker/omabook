<!-- Omabook and Calibre: https://jellespijker.github.io/omabook/docs/omabook-vs-calibre.html -->

# Omabook and Calibre

Calibre is the long-standing ebook manager, and Omabook is built to take over from it for reading and keeping a library, not to replace everything it does. The two work side by side: Omabook only reads Calibre’s folder.

## What Omabook is built around

- A knowledge graph and timelines of the people, places, ideas and events of what you read, never past your page.
- A reader that sets the page for comfortable reading, with two or three pages side by side where the window has room.
- AI per capability (answers, quick work, handwriting, speech to text, text to speech, embeddings), local first, with a Test button on every row.
- Voice questions and commands while reading, dictated notes, and handwriting in the margins.
- Following the Omarchy theme and language live, and a keyboard way of working.
- A command line with JSON output and an MCP server, so agents can use the library.

## What comes across from Calibre

Your curated details, covers, all formats, stars, how far you read, identifiers (Goodreads, ISBN, Amazon), the viewer’s highlights and bookmarks, and the columns people add (read, status, date read). Calibre’s metadata source plugins run inside Calibre for look-ups, and its conversion plugins run through `ebook-convert`. See [Calibre](calibre.html).

## What Omabook leaves to Calibre

- Conversion tuning and the Calibre-only formats: Omabook converts through Calibre’s `ebook-convert` when it needs to.
- Calibre’s device drivers and store plugins, and the pieces that are part of Calibre’s own window. Omabook sends books to e-readers in its own way: see [phones and e-readers](phones-and-e-readers.html).

## Trying it safely

Importing reads Calibre’s folder and never changes it. Import again later and Omabook brings its copy up to date without adding a book twice. You can keep using Calibre as before.
