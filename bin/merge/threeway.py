"""Three-way merges of the shared files' data, for the merge drivers beside this file.

Git hands a driver the file as it was where the two sides parted (the base), as
this side has it (ours) and as the other side has it (theirs). What only one
side changed is taken as that side has it; a mapping is merged key by key and a
list of names name by name, so two changes that each add a key or a name never
conflict. What both sides changed, each its own way, the driver decides
(`both_changed`), or it raises Conflict and the file is merged line by line,
with conflict markers where the lines meet, as git would have.
"""

from __future__ import annotations

import subprocess
import sys
from collections.abc import Callable
from pathlib import Path


class Missing:
    """A key one side doesn't have: never there, or removed."""

    def __repr__(self) -> str:
        return "MISSING"


MISSING = Missing()


class Conflict(Exception):
    """Both sides changed one value, each its own way, and neither can be chosen."""


BothChanged = Callable[[object, object, object], object]


def merge(base: object, ours: object, theirs: object, both_changed: BothChanged) -> object:
    """The value both sides' changes to `base` lead to."""
    if ours == theirs:
        return ours
    if ours == base:
        return theirs
    if theirs == base:
        return ours
    if isinstance(ours, dict) and isinstance(theirs, dict):
        return merge_mappings(base if isinstance(base, dict) else {}, ours, theirs, both_changed)
    if isinstance(ours, list) and isinstance(theirs, list) and all(isinstance(item, str) for item in ours + theirs):
        return merge_names(base if isinstance(base, list) else [], ours, theirs)
    return both_changed(base, ours, theirs)


def merge_mappings(base: dict, ours: dict, theirs: dict, both_changed: BothChanged) -> dict:
    """Key by key, in our order with theirs' new keys after; sorted where ours was."""
    keys = list(ours) + [key for key in theirs if key not in ours]
    merged = {}
    for key in keys:
        value = merge(base.get(key, MISSING), ours.get(key, MISSING), theirs.get(key, MISSING), both_changed)
        if value is not MISSING:
            merged[key] = value
    if len(ours) > 1 and list(ours) == sorted(ours):
        return dict(sorted(merged.items()))
    return merged


def merge_names(base: list[str], ours: list[str], theirs: list[str]) -> list[str]:
    """Our names but those theirs removed, then the names theirs added, each once."""
    kept = [name for name in ours if name in theirs or name not in base]
    return kept + [name for name in theirs if name not in base and name not in kept]


def run(merge_text: Callable[[str, str, str], str], argv: list[str]) -> int:
    """A driver's main: `driver %O %A %B`, the result written over %A (ours).

    0 when merged; otherwise the line-by-line merge's count of conflicts, as git
    merge-file leaves it in %A with its markers.
    """
    base, ours, theirs = (Path(arg) for arg in argv[:3])
    texts = [path.read_text(encoding="utf-8") for path in (base, ours, theirs)]
    try:
        merged = merge_text(*texts)
    except (Conflict, ValueError) as reason:
        print(f"{argv[3] if len(argv) > 3 else ours}: merged line by line ({reason})", file=sys.stderr)
        labels = ["-L", "ours", "-L", "base", "-L", "theirs"]
        return subprocess.run(["git", "merge-file", *labels, str(ours), str(base), str(theirs)], check=False).returncode
    ours.write_text(merged, encoding="utf-8")
    return 0
