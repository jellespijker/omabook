# 5. Failures below the front ends are codes, worded where they are shown

- Status: accepted (#70)
- Date: 2026-11-20

## Context

The window follows the desktop's language, but the reasons the layers below it gave (a file that can't
be read, a catalog that can't be reached) were English sentences written where the failure happened, and
reached the window as they were. The command line and the MCP server show the same reasons to scripts and
agents, which read them as English and should keep doing so.

## Decision

- Below the front ends (`foundation` to `operations`), a reason is a **`Failure`** (`src/failure.h`): a
  **code** and its **arguments** (a file's name, a catalog's, the system's own reason), and a count where
  the sentence counts something. It travels as `std::expected<T, Failure>` or a signal's argument.
- It is **worded by `Failure::text()`**, from one table in `failure.cpp` through the catalogs in
  `src/i18n` (plural forms included). `text()` translates through whatever translator the process
  installed: the window installs the desktop's, so it reads in Dutch, German, French or Spanish there; a
  command and the MCP server install none and read English, unless a command is given `--translated`.
- Words from elsewhere (a plugin, a server, a program) are passed on as they were said, as
  `Failure::said(words)`.
- Where an interface still carries a `QString` (`Formats::open`'s out-parameter, `Library::Added::error`,
  an import's problems), the `Failure` is worded at that seam with `text()`, which is still the front end's
  language. Such seams move to `Failure` when they are touched for a reason.
- `check_wording.py` flags a new English sentence handed on as a failure (`std::unexpected("...")`,
  `emit failed("...")`, `.error = "..."`, `problems << "..."`) in those layers.

## Considered options

- **`tr()` where the failure happens**: the same result at run time, but sentences scattered over every
  module, and nothing a caller can tell one reason from another by.
- **Codes the front ends each word**: the window and the command line would each need every sentence,
  and the English twice over.
- **Keys such as `error.file-unreadable` in the catalogs**: the catalogs are keyed by their English text
  (`bin/i18n`), and translators work from the sentence.

## Consequences

- A new kind of failure is a code, its sentence in `failure.cpp`, and the sentence in the five catalogs.
- Opening a book, asking a catalog or a plugin's source for a book's details, and adding books are
  converted. Other modules keep their English until they are touched; the AI's failures are matched on
  their English words (whether to try the main model) and are converted when that matching is not.
- Logs stay English: they are for whoever reads the log, not the person at the window.
