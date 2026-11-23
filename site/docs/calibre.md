<!-- Calibre: https://jellespijker.github.io/omabook/docs/calibre.html -->

# Calibre

Everything Omabook does with Calibre is one plugin, **Calibre**, at the top of the plugin list. Switched off, nothing of Calibre’s is run, read or offered.

## Import

- Finds your Calibre libraries and offers to import them. Calibre’s folder is only read.
- Every book comes with its curated details, cover and formats, your stars, your place, and the viewer’s highlights and bookmarks.
- Calibre’s metadata plugins are found and used for look-ups. Conversion plugins run through ebook-convert.
- Import again later and it brings things up to date without adding a book twice.

Omabook reads Calibre’s own settings and looks in `~/Calibre Library`. A library that was never imported is offered once at the bottom of the shelf: *Import*, *Not now* or *Never*. In a terminal: `omabook calibre libraries` and `omabook calibre import`.

## What comes along

The details you curated, the cover, all formats, your stars, how far you read each book (a book read to the end is read), identifiers (Goodreads, ISBN, Amazon), the viewer’s highlights with their notes, and its bookmarks. Columns people add are recognised by name (`read`, `status`, `date_read`, `myrating`, the Goodreads Sync columns).

## Calibre’s plugins

Metadata sources (Amazon, Goodreads, Kobo and others) are run inside Calibre and can each be switched on or off in the look-up (`M`). Conversion plugins run through `ebook-convert`. Device drivers and file type plugins are listed with the reason they are not used. Sending to an e-reader is covered by Omabook’s own [device support](phones-and-e-readers.html).
