#!/usr/bin/env python3
"""Merge driver for the translation catalogs, src/i18n/*.json (see bin/setup).

Each side only adds, changes or removes strings, so the catalogs are merged key
by key: every string either side added, without those either side removed. A
string both sides translated, each its own way, keeps theirs: in a rebase that
is the commit being replayed, which knows its own words. A string one side
removed (its English changed or went) and the other translated stays removed.
Keys stay sorted, written as bin/i18n writes them. Afterwards bin/i18n still
decides what en.json holds: run it, as ever, when the sources' strings changed.
"""

from __future__ import annotations

import json
import sys

from threeway import MISSING, merge, run


def removed_or_theirs(_base: object, ours: object, theirs: object) -> object:
    return MISSING if MISSING in (ours, theirs) else theirs


def merge_catalogs(base: str, ours: str, theirs: str) -> str:
    parsed = [json.loads(text) if text.strip() else {} for text in (base, ours, theirs)]
    merged = merge(*parsed, removed_or_theirs)
    return json.dumps(merged, ensure_ascii=False, indent=2) + "\n"


if __name__ == "__main__":
    sys.exit(run(merge_catalogs, sys.argv[1:]))
