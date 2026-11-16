#!/usr/bin/env python3
"""clang-format, on the lines a change adds or modifies.

Most of the code was formatted by hand before .clang-format existed, so the
whole files are not checked: only what a change writes. `git clang-format`
formats exactly those lines.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys

from harness import CPP_SUFFIXES, ROOT, Scope, report


def changed_lines(scope: Scope, paths: list[str]) -> list[str]:
    """git clang-format on files that existed before: only the lines the change touched."""
    command = ["git", "clang-format", "--diff", "--quiet"]
    command += ["--staged", scope.base] if scope.mode == "staged" else [scope.base]
    result = subprocess.run([*command, "--", *paths], cwd=ROOT, capture_output=True, text=True)
    if not (result.returncode and result.stdout.strip()):
        return []
    print(result.stdout, file=sys.stderr)
    return [line for line in result.stdout.splitlines() if line.startswith("diff --git")]


def whole_file(scope: Scope, path: str) -> list[str]:
    """A file the change adds is checked whole."""
    result = subprocess.run(
        ["clang-format", "--dry-run", "--Werror", f"--assume-filename={path}"],
        cwd=ROOT,
        input=scope.after(path),
        capture_output=True,
        text=True,
    )
    return [f"{path}: not formatted (a new file is checked whole)"] if result.returncode else []


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    Scope.add_arguments(parser)
    scope = Scope.from_arguments(parser.parse_args(argv))
    files = scope.files(CPP_SUFFIXES)
    if not files:
        return 0
    if not shutil.which("clang-format") or not shutil.which("git-clang-format"):
        print("check_format: clang-format is not installed; skipped.", file=sys.stderr)
        return 0 if scope.mode == "changed" else 1
    new = [path for path in files if scope.before(path) is None]
    old = [path for path in files if path not in new]
    findings = changed_lines(scope, old) if old else []
    for path in new:
        findings += whole_file(scope, path)
    return report("clang-format", findings, "run `git clang-format` (changed lines) or `clang-format -i <new file>`")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
