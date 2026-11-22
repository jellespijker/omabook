#!/usr/bin/env python3
"""Merge driver for .agents/architecture.toml, the layers of src/ (see bin/setup).

Tickets add modules to a layer, mostly at the end of its list, so two of them
meet on the same line. This merges the file by its tables and keys instead: a
layer's modules are every module either side placed there, without those either
side took out (a module moved to another layer leaves the first). A value only
one side changed is taken as it has it; one both changed, each its own way,
(a layer's summary, its uses) is merged line by line, with conflict markers.

The file is written as ours has it, with only the changed lines touched: names
taken out of their line, names added on a line of their own before the `]`.
"""

from __future__ import annotations

import re
import sys
import tomllib
from dataclasses import dataclass

from threeway import MISSING, Conflict, merge, run

HEADER = re.compile(r"^\[([\w.-]+)\]\s*(#.*)?$")
KEY = re.compile(r"^([\w-]+)\s*=")
WIDTH = 118


@dataclass
class Table:
    """A table's lines, its header first; the lines before the first header are the table ""."""

    name: str
    lines: list[str]


def tables(text: str) -> list[Table]:
    found = [Table("", [])]
    for line in text.splitlines():
        if match := HEADER.match(line):
            found.append(Table(match.group(1), []))
        found[-1].lines.append(line)
    return found


def bracket_depth(line: str) -> int:
    """How many more arrays `line` opens than it closes, outside strings and comments."""
    depth, quote, escaped = 0, "", False
    for char in line:
        if escaped:
            escaped = False
        elif quote:
            escaped = quote == '"' and char == "\\"
            quote = "" if char == quote else quote
        elif char in "\"'":
            quote = char
        elif char == "#":
            break
        else:
            depth += {"[": 1, "]": -1}.get(char, 0)
    return depth


def entries(lines: list[str]) -> dict[str, tuple[int, int]]:
    """Each key of a table, with the lines its value spans."""
    spans, index = {}, 0
    while index < len(lines):
        match = KEY.match(lines[index])
        if not match:
            index += 1
            continue
        start, depth = index, bracket_depth(lines[index])
        index += 1
        while depth > 0 and index < len(lines):
            depth += bracket_depth(lines[index])
            index += 1
        spans[match.group(1)] = (start, index)
    return spans


def lookup(data: object, name: str) -> object:
    for part in name.split(".") if name else []:
        data = data.get(part, MISSING) if isinstance(data, dict) else MISSING
    return data


def quoted(name: str) -> str:
    return '"' + name.replace("\\", "\\\\").replace('"', '\\"') + '"'


def take_out(lines: list[str], name: str) -> list[str]:
    token = re.escape(quoted(name))
    for index, line in enumerate(lines):
        if quoted(name) in line:
            line = re.sub(token + r",?[ \t]*", "", line, count=1)
            lines[index] = re.sub(r",\s*\]", "]", line)
            break
    return lines if len(lines) == 1 else [line for line in lines if line.strip()]


def wrapped(indent: str, names: list[str]) -> list[str]:
    out, current = [], indent
    for name in names:
        piece = quoted(name) + ","
        if len(current) + 1 + len(piece) > WIDTH and current.strip():
            out.append(current)
            current = indent
        current += (" " if current.strip() else "") + piece
    return out + [current]


def patch_names(lines: list[str], old: list[str], new: list[str]) -> list[str]:
    """`lines` (a list of names) with what `new` drops taken out and what it adds put in."""
    lines = list(lines)
    for name in (name for name in old if name not in new):
        lines = take_out(lines, name)
    added = [name for name in new if name not in old]
    if not added:
        return lines
    if len(lines) == 1:
        line = lines[0]
        close = line.rindex("]")
        before = line[:close].rstrip()
        separator = "" if before.endswith("[") else (" " if before.endswith(",") else ", ")
        return [before + separator + ", ".join(quoted(name) for name in added) + line[close:]]
    indent = re.match(r"^\s*", lines[1]).group(0) if len(lines) > 2 else "  "
    if len(lines) > 2 and not lines[-2].rstrip().endswith(","):
        lines[-2] = lines[-2].rstrip() + ","
    return lines[:-1] + wrapped(indent, added) + lines[-1:]


def names(value: object) -> bool:
    return isinstance(value, list) and all(isinstance(item, str) for item in value)


def patch_table(table: Table, values: dict, ours: dict, theirs: Table | None) -> list[str]:
    """`table` as ours has it, changed where the merged `values` differ from `ours`."""
    lines, spans = list(table.lines), entries(table.lines)
    theirs_lines = theirs.lines if theirs else []
    theirs_spans = entries(theirs_lines)

    def from_theirs(key: str) -> list[str]:
        if key not in theirs_spans:
            raise Conflict(f"{key} in [{table.name}] changed on both sides")
        start, end = theirs_spans[key]
        return theirs_lines[start:end]

    for key, (start, end) in sorted(spans.items(), key=lambda item: -item[1][0]):
        new = values.get(key, MISSING)
        if new == ours.get(key):
            continue
        if new is MISSING:
            replacement = []
        elif names(new) and names(ours.get(key)):
            replacement = patch_names(lines[start:end], ours[key], new)
        else:
            replacement = from_theirs(key)
        lines[start:end] = replacement
    added = [key for key in theirs_spans if key not in spans and values.get(key, MISSING) is not MISSING]
    end = max((span[1] for span in entries(lines).values()), default=len(lines))
    lines[end:end] = [line for key in added for line in from_theirs(key)]
    return lines


def render(ours_text: str, theirs_text: str, ours: dict, merged: dict) -> str:
    ours_tables, theirs_tables = tables(ours_text), {table.name: table for table in tables(theirs_text)}
    out: list[str] = []
    for table in ours_tables:
        values = lookup(merged, table.name)
        if isinstance(values, dict):
            out += patch_table(table, values, lookup(ours, table.name), theirs_tables.get(table.name))
    known = {table.name for table in ours_tables}
    for name, table in theirs_tables.items():
        if name not in known and isinstance(lookup(merged, name), dict):
            out += ([""] if out and out[-1].strip() else []) + table.lines
    return "\n".join(out).rstrip("\n") + "\n"


def refuse(_base: object, ours: object, theirs: object) -> object:
    raise Conflict(f"both sides changed {ours!r} and {theirs!r}")


def merge_architecture(base: str, ours: str, theirs: str) -> str:
    try:
        parsed = [tomllib.loads(text) for text in (base, ours, theirs)]
    except tomllib.TOMLDecodeError as error:
        raise Conflict(f"not TOML: {error}") from error
    merged = merge(*parsed, refuse)
    text = render(ours, theirs, parsed[1], merged)
    if tomllib.loads(text) != merged:
        raise Conflict("the merged tables could not be written as ours has them")
    return text


if __name__ == "__main__":
    sys.exit(run(merge_architecture, sys.argv[1:]))
