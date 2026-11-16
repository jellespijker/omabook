#!/usr/bin/env python3
"""clang-tidy (.clang-tidy), on the lines a change adds.

Needs a build's compile_commands.json (build/, or $OMABOOK_BUILD). Without one
the check is skipped locally and fails in CI (--base). A changed header is
checked through the source file of the same name.

clang-tidy is slow (seconds per file), so the agents' edit hook leaves it out;
the pre-commit hook and CI run it. To see everything in a file, not just the
change: `clang-tidy -p build src/<file>.cpp`.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from harness import CPP_SUFFIXES, ROOT, Scope, report, touches

DIAGNOSTIC = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+):\d+: (?P<level>warning|error): (?P<message>.*) \[(?P<check>[^\]]+)\]$")
# Flags GCC understands and clang does not; Qt adds them to every target.
GCC_ONLY = ("-mno-direct-extern-access", "-fno-direct-access-external-data")


def database() -> Path | None:
    """A copy of the build's compile commands that clang can read."""
    build = Path(os.environ.get("OMABOOK_BUILD", ROOT / "build"))
    source = build / "compile_commands.json"
    if not source.exists():
        return None
    target = build / "harness"
    target.mkdir(exist_ok=True)
    commands = json.loads(source.read_text(encoding="utf-8"))
    for entry in commands:
        if "command" in entry:
            entry["command"] = " ".join(a for a in entry["command"].split(" ") if a not in GCC_ONLY)
        if "arguments" in entry:
            entry["arguments"] = [a for a in entry["arguments"] if a not in GCC_ONLY]
    (target / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")
    return target


def unit_for(path: str) -> str | None:
    """The source file clang-tidy compiles to see `path`."""
    if path.endswith(".cpp"):
        return path
    source = Path(path).with_suffix(".cpp")
    return source.as_posix() if (ROOT / source).exists() else None


def jobs() -> int:
    result = subprocess.run([str(ROOT / "bin/jobs")], capture_output=True, text=True)
    return max(1, int(result.stdout.strip() or 1))


def run_tidy(db: Path, units: list[str]) -> str:
    def tidy(unit: str) -> str:
        result = subprocess.run(["clang-tidy", "-p", str(db), "--quiet", unit], cwd=ROOT, capture_output=True, text=True)
        return result.stdout

    with ThreadPoolExecutor(max_workers=jobs()) as pool:
        return "\n".join(pool.map(tidy, units))


def findings_in(output: str, selected: dict[str, set[int] | None]) -> list[str]:
    """The diagnostics that fall on lines the change adds."""
    found = set()
    for line in output.splitlines():
        if not (match := DIAGNOSTIC.match(line)):
            continue
        path = Path(match["file"]).resolve()
        relative = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else ""
        number = int(match["line"])
        if relative in selected and touches(selected[relative], number, number):
            found.add(f"{relative}:{number}: [{match['check']}] {match['message']}")
    return sorted(found)


def skipped(reason: str, scope: Scope) -> int:
    print(f"check_clang_tidy: {reason}; skipped.", file=sys.stderr)
    return 1 if scope.mode == "base" else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    Scope.add_arguments(parser)
    scope = Scope.from_arguments(parser.parse_args(argv))
    selected = {path: scope.added_lines(path) for path in scope.files(CPP_SUFFIXES) if path.startswith("src/")}
    selected = {path: lines for path, lines in selected.items() if lines != set()}
    units = sorted({unit for path in selected if (unit := unit_for(path))})
    if not units:
        return 0
    if not shutil.which("clang-tidy"):
        return skipped("clang-tidy is not installed", scope)
    db = database()
    if db is None:
        return skipped("no build/compile_commands.json (run bin/build)", scope)
    findings = findings_in(run_tidy(db, units), selected)
    return report("clang-tidy", findings, "fix it, or say why not with `// NOLINT(<check>): <reason>`")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
