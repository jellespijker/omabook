#!/usr/bin/env python3
"""clang-format: a new file is formatted whole; in an old file, the changed lines are a suggestion.

Most of the code was formatted by hand before .clang-format existed, in a
style clang-format comes near but can't reproduce: it puts each element of a
nested braced list on a line of its own where the code packs them, so a
changed `QJsonObject{{...}, {...}}` would grow by a line an element and run
into the file-size ratchet. So in a file that existed, the formatting of the
changed lines is shown as a diff to consider (`git clang-format` applies it)
and does not fail; match the code around it. A new file has no style of its
own yet, and must be clang-formatted (`clang-format -i <file>`).
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
    if old and (suggested := changed_lines(scope, old)):
        report(
            "clang-format (a suggestion, not blocking)", suggested, "match the code around it; `git clang-format` applies the diff above"
        )
    findings = [finding for path in new for finding in whole_file(scope, path)]
    return report("clang-format", findings, "run `clang-format -i <new file>`")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
