#!/usr/bin/env python3
"""The change log's notes: a change leaves a fragment in changes/, never an edit to CHANGELOG.md.

Two tickets that each wrote a line under [Unreleased] met on the same lines;
a fragment of their own (changes/<issue>.md, see bin/release-notes) never does.

- CHANGELOG.md gains lines, and its released sections change, only in a release:
  a commit named "Release <version>: ...", or a change that raises
  `project(omabook VERSION ...)` in CMakeLists.txt. Outside one, a change may
  only take lines out of [Unreleased] (into fragments) and reword the preamble.
- A change to what Omabook does (src/, share/) adds a fragment: its line for the
  change log, or `section: none` saying why no one using Omabook would notice.
- Every fragment it adds is one bin/release-notes can read.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

from harness import ROOT, Scope, git, report

BEHAVIOUR = ("src/", "share/")
VERSION = re.compile(r"project\(omabook VERSION ([\d.]+)")
UNRELEASED = "## [Unreleased]"
RELEASE = re.compile(r"^Release \d+(\.\d+)+\b")


def version(text: str | None) -> str | None:
    match = VERSION.search(text or "")
    return match.group(1) if match else None


def is_release(scope: Scope) -> bool:
    if (ROOT / "CMakeLists.txt").is_file() and version(scope.before("CMakeLists.txt")) != version(scope.after("CMakeLists.txt")):
        return True
    if scope.mode != "base":
        return False
    subjects = git("log", "--format=%s", f"{scope.base}..HEAD").splitlines()
    return any(RELEASE.match(subject) for subject in subjects)


def parts(text: str) -> tuple[list[str], list[str]]:
    """The lines under [Unreleased], and those of the released sections after it."""
    lines = text.splitlines()
    start = lines.index(UNRELEASED) if UNRELEASED in lines else len(lines)
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## [")), len(lines))
    return [line for line in lines[start + 1 : end] if line.strip() and not line.startswith("### ")], lines[end:]


def changelog_findings(scope: Scope) -> list[str]:
    before, after = parts(scope.before("CHANGELOG.md") or ""), parts(scope.after("CHANGELOG.md"))
    findings = []
    if any(line not in before[0] for line in after[0]):
        findings.append("CHANGELOG.md: lines added under [Unreleased]; put the change's lines in changes/<issue>.md instead")
    if before[1] != after[1]:
        findings.append("CHANGELOG.md: a released section changed outside a release")
    return findings


def changed(scope: Scope) -> list[str]:
    """Every file the change adds, modifies or deletes."""
    args = ["--cached", scope.base] if scope.mode == "staged" else [scope.base]
    names = set(git("diff", "--name-only", *args).split())
    if scope.mode != "staged":
        names |= set(git("ls-files", "--others", "--exclude-standard").split())
    return sorted(names)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    Scope.add_arguments(parser)
    scope = Scope.from_arguments(parser.parse_args(argv))
    if is_release(scope):
        return 0
    files = changed(scope)
    fragments = [path for path in scope.files() if path.startswith("changes/") and path.endswith(".md")]
    findings = []
    if "CHANGELOG.md" in files and (ROOT / "CHANGELOG.md").is_file():
        findings += changelog_findings(scope)
    behaviour = [path for path in files if path.startswith(BEHAVIOUR)]
    if behaviour and not fragments:
        more = f" and {len(behaviour) - 1} more" if len(behaviour) > 1 else ""
        findings.append(f"{behaviour[0]}{more}: what Omabook does changes, and no changes/<issue>.md says how")
    if fragments:
        checked = subprocess.run(
            [sys.executable, str(ROOT / "bin/release-notes"), "--check", *fragments], cwd=ROOT, capture_output=True, text=True
        )
        findings += checked.stderr.splitlines()
    return report(
        "changelog",
        findings,
        "a change's notes go in changes/<issue>.md (front matter `section: Added|Changed|Fixed|...`, then `- the line (#n)`); "
        "`section: none` with the reason for a change no one would notice; see AGENTS.md, Change notes",
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
