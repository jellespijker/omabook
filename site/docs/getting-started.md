<!-- Getting started: https://jellespijker.github.io/omabook/docs/getting-started.html -->

# Getting started

Omabook is an ebook library and reader for Omarchy. It runs on Arch and Omarchy (x86_64) and needs Qt 6.

## Install

Each release has a ready-built package for Arch and Omarchy. Download the newest from the [releases page](https://github.com/jellespijker/omabook/releases), check its SHA-256 against the one listed there, and install it with pacman. The package is downloaded first because pacman asks for a signature file next to a package it fetches from a link, and these packages are not signed.

```
sha256sum omabook-*.pkg.tar.zst
sudo pacman -U omabook-*.pkg.tar.zst
```

Open it from the launcher, or run `omabook open`.

## Add books

Press `A` to add books, `Shift+A` to add a folder, or drop files on the window. Books are copied into `~/Books` under `Author/Title (id)/` with their cover beside them and one SQLite file for the rest. The file you added is never touched.

If you have Calibre, Omabook finds its libraries and offers to import them. See [Calibre](calibre.html).

## Read

Press `Space` on a book. EPUB, FictionBook, Markdown, HTML and plain text flow in your own font and the desktop’s colors. PDF and comics are shown page by page. Kindle, Word, OpenDocument, RTF, DjVu and the other formats Calibre knows are converted to EPUB the first time you open them, which needs Calibre’s `ebook-convert`.

[Picture: The shelf as covers with a book's details beside it.]

The shelf.

## Learn the keys

Press `?` for every key as it is bound now, and for short pages on how things work. The tour is a walk through the real window and is always under `?`. In a terminal: `omabook help`.
