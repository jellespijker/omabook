#!/usr/bin/env python3
"""No new English sentences for people below the front ends (ADR 0005), on the lines a change adds.

A module in a layer under `commands` and `window` (.agents/architecture.toml)
says why something failed with a `Failure` (src/failure.h): a code and its
arguments, worded where it is shown, in the desktop's language in the window.
A sentence written there instead would reach the window in English, so a new
one is a finding where it is handed on as a failure:

  std::unexpected("..."), emit failed("..."), .error = "...", problems << "..."

A sentence is a string literal of two words or more. failure.cpp, which words
the codes, is where the sentences belong. Log lines (qCWarning and the rest)
are for whoever reads the log and stay English.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from check_architecture import Layers
from harness import CPP_SUFFIXES, Scope, report, touches

FRONT_ENDS = frozenset({"commands", "window", "app"})
WORDING = frozenset({"failure"})  # the modules that word failures
HANDED_ON = re.compile(r"std::unexpected\s*\(|\bemit\s+\w*[fF]ailed\s*\(|\.error\s*=(?!=)|\bproblems\s*<<")
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
WORDS = re.compile(r"[A-Za-z]{2,}\s+\S*[A-Za-z]")


def statement_end(text: str, start: int) -> int:
    """Where the statement that starts at `start` ends: its `;` outside strings, or the end of the text."""
    i, quote = start, ""
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == quote:
                quote = ""
        elif c == '"':
            quote = c
        elif c == ";":
            return i + 1
        i += 1
    return len(text)


def findings(path: str, text: str, added: set[int] | None) -> list[str]:
    found = []
    for match in HANDED_ON.finditer(text):
        end = statement_end(text, match.start())
        sentence = next((s for s in LITERAL.finditer(text, match.end(), end) if WORDS.search(s.group(1))), None)
        if not sentence:
            continue
        line = text.count("\n", 0, match.start()) + 1
        if touches(added, line, text.count("\n", 0, end) + 1):
            found.append(f"{path}:{line}: an English sentence handed on as a failure: {sentence.group(0)[:60]}")
    return found


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    Scope.add_arguments(parser)
    scope = Scope.from_arguments(parser.parse_args(argv))
    layers = Layers.load()
    found = []
    for path in scope.files(CPP_SUFFIXES):
        module = Path(path).stem
        layer = layers.layer_of.get(module)
        if not path.startswith("src/") or layer is None or layer in FRONT_ENDS or module in WORDING:
            continue
        added = scope.added_lines(path)
        if added != set():
            found += findings(path, scope.after(path), added)
    return report(
        "wording",
        found,
        "say why with a Failure (src/failure.h): add a code, its sentence in failure.cpp and the five catalogs (bin/i18n)",
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
