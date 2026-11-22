#!/usr/bin/env python3
"""Merge driver for the harness's baselines, .agents/*-baseline.json (see bin/setup).

A baseline only ever shrinks: each side lowers a ceiling, or drops an entry once
it is within budget. So they are merged key by key, and an entry by its value:
where both sides changed one ceiling, the lower stays, and an entry one side
dropped stays dropped. A list (the architecture's grandfathered includes) keeps
what neither side removed. The checks lower the ceilings further on their next run.
"""

from __future__ import annotations

import json
import sys

from threeway import MISSING, Conflict, merge, run


def lower(_base: object, ours: object, theirs: object) -> object:
    if MISSING in (ours, theirs):
        return MISSING
    if isinstance(ours, int | float) and isinstance(theirs, int | float):
        return min(ours, theirs)
    raise Conflict(f"both sides changed {ours!r} and {theirs!r}")


def merge_baselines(base: str, ours: str, theirs: str) -> str:
    parsed = [json.loads(text) if text.strip() else {} for text in (base, ours, theirs)]
    return json.dumps(merge(*parsed, lower), indent=2) + "\n"


if __name__ == "__main__":
    sys.exit(run(merge_baselines, sys.argv[1:]))
