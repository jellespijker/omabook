#!/usr/bin/env python3
"""The tools that keep tickets from meeting on shared files (#198), each on fixtures.

- bin/release-notes turns fragments into a release's section of the change log;
- the merge drivers in bin/merge/, registered by bin/setup, merge the catalogs,
  the architecture and the baselines three-way in a real git merge;
- the build finds a test suite by its name, with no list to add it to.

Every test works in a scratch folder. Run: python3 tests/workflow_tests.py
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def isolated() -> dict[str, str]:
    """This environment without git's own variables, which would point git at another repository."""
    return {name: value for name, value in os.environ.items() if not name.startswith("GIT_")}


def dedent(text: str) -> str:
    return textwrap.dedent(text).lstrip("\n")


class Scratch(unittest.TestCase):
    def setUp(self) -> None:
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)

    def tearDown(self) -> None:
        self.folder.cleanup()

    def write(self, path: str, text: str) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(dedent(text), encoding="utf-8")

    def read(self, path: str) -> str:
        return (self.root / path).read_text(encoding="utf-8")

    def run_in(self, *command: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(command, cwd=cwd or self.root, capture_output=True, text=True, env=isolated())


CHANGELOG = """
    # Changelog

    The preamble.

    ## [Unreleased]

    ## [0.3.2] - 2026-11-22

    ### Fixed

    - An old fix (#166).
    """


class ReleaseNotes(Scratch):
    def setUp(self) -> None:
        super().setUp()
        (self.root / "bin").mkdir()
        shutil.copy2(REPO / "bin/release-notes", self.root / "bin/release-notes")
        self.write("CHANGELOG.md", CHANGELOG)

    def notes(self, *args: str) -> subprocess.CompletedProcess:
        return self.run_in(sys.executable, str(self.root / "bin/release-notes"), *args)

    def test_fragments_become_the_versions_section(self) -> None:
        self.write("changes/151.md", "---\nsection: Added\n---\n\n- Recordings written out (#151).\n")
        self.write("changes/12.md", "---\nsection: Fixed\n---\n\n- A sync keeps its covers (#12).\n")
        self.write(
            "changes/9.md",
            "---\nsection: Added\n---\n\n- Fold the margin away (#9),\n  on every book.\n- Its dots (#9).\n",
        )
        self.write("changes/9-refactor.md", "---\nsection: none\n---\n\n- Only the code moved.\n")
        self.write("changes/40.md", "---\nsection: Changed\n---\n\n- Voices read on (#40).\n")
        result = self.notes("0.4.0", "--date", "2026-12-01")
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = """
            # Changelog

            The preamble.

            ## [Unreleased]

            ## [0.4.0] - 2026-12-01

            ### Added

            - Fold the margin away (#9),
              on every book.
            - Its dots (#9).
            - Recordings written out (#151).

            ### Changed

            - Voices read on (#40).

            ### Fixed

            - A sync keeps its covers (#12).

            ## [0.3.2] - 2026-11-22

            ### Fixed

            - An old fix (#166).
            """
        self.assertEqual(self.read("CHANGELOG.md"), dedent(expected))
        self.assertEqual(list((self.root / "changes").iterdir()), [])

    def test_lines_still_under_unreleased_come_along(self) -> None:
        self.write("CHANGELOG.md", CHANGELOG.replace("## [Unreleased]\n", "## [Unreleased]\n\n    ### Fixed\n\n    - Kept (#3).\n"))
        self.write("changes/5.md", "---\nsection: Fixed\n---\n- Added (#5).\n")
        self.assertEqual(self.notes("0.4.0", "--date", "2026-12-01").returncode, 0)
        self.assertIn("## [0.4.0] - 2026-12-01\n\n### Fixed\n\n- Kept (#3).\n- Added (#5).\n\n## [0.3.2]", self.read("CHANGELOG.md"))

    def test_a_released_version_or_nothing_to_release_is_refused(self) -> None:
        self.write("changes/5.md", "---\nsection: Fixed\n---\n- Added (#5).\n")
        self.assertIn("already has 0.3.2", self.notes("0.3.2").stderr)
        (self.root / "changes/5.md").unlink()
        self.assertIn("nothing to release", self.notes("0.4.0").stderr)
        self.assertEqual(self.read("CHANGELOG.md"), dedent(CHANGELOG))

    def test_check_says_what_is_wrong_with_a_fragment(self) -> None:
        self.write("changes/1.md", "---\nsection: Added\n---\n- Fine (#1).\n")
        self.write("changes/2.md", "---\nsection: Improved\n---\n- Fine (#2).\n")
        self.write("changes/3.md", "---\nsection: Fixed\n---\n\n")
        self.write("changes/notes.md", "---\nsection: Fixed\n---\n- Fine.\n")
        self.write("changes/4.md", "- No front matter (#4).\n")
        self.write("changes/5.md", "---\nsection: Fixed\n---\nA sentence without its dash.\n")
        result = self.notes("--check")
        self.assertEqual(result.returncode, 1)
        for name, problem in [("2", "section is Improved"), ("3", "has no line"), ("notes", "name it"), ("4", "front matter"), ("5", "- ")]:
            self.assertIn(f"changes/{name}.md: ", result.stderr)
            self.assertIn(problem, result.stderr)
        self.assertNotIn("changes/1.md", result.stderr)
        self.assertEqual(self.notes("--check", str(self.root / "changes/1.md")).returncode, 0)


ARCHITECTURE = dedent(
    """
    # The layers.

    [layers.foundation]
    summary = "Settings."
    uses = []
    modules = [
      "crypto", "hlc", "log",
      "settings", "theme",
    ]

    # The front ends.
    [layers.commands]
    summary = "The command line."
    uses = ["foundation"]
    modules = ["cli", "mcp"]

    [qt]
    window_only = ["QQuick"]
    """
)


class MergeDrivers(Scratch):
    """A merge of two branches that each changed a shared file, as bin/setup leaves git."""

    def setUp(self) -> None:
        super().setUp()
        for item in ("bin/setup", "bin/merge", ".gitattributes"):
            source, target = REPO / item, self.root / item
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, target, ignore=shutil.ignore_patterns("__pycache__")) if source.is_dir() else shutil.copy2(
                source, target
            )
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "workflow@example.invalid")
        self.git("config", "user.name", "Workflow test")
        self.git("config", "rerere.enabled", "false")
        self.assertEqual(self.run_in("bin/setup", "--merge-drivers").returncode, 0)

    def git(self, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        result = self.run_in("git", *args, cwd=cwd)
        if result.returncode and args[0] != "merge":
            self.fail(f"git {' '.join(args)}: {result.stderr}")
        return result

    def commit(self, files: dict[str, str]) -> None:
        for path, text in files.items():
            self.write(path, text)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "A side (#198)")

    def merged(self, base: dict[str, str], ours: dict[str, str], theirs: dict[str, str]) -> subprocess.CompletedProcess:
        self.commit(base)
        self.git("checkout", "-q", "-b", "theirs")
        self.commit(theirs)
        self.git("checkout", "-q", "main")
        self.commit(ours)
        # From a folder below the top, as a person may: the drivers still run.
        (self.root / "src").mkdir(exist_ok=True)
        return self.git("merge", "-q", "--no-edit", "theirs", cwd=self.root / "src")

    def test_catalogs_merge_key_by_key(self) -> None:
        def catalog(entries: dict) -> str:
            return json.dumps(entries, ensure_ascii=False, indent=2) + "\n"

        base = {"Back": "Zurück", "Close": "Schließen", "Old": "Alt"}
        ours = {"Back": "Zurück", "Close": "Schließen", "New": "Neu"}
        theirs = {"Back": "Zurück", "Close": "Zumachen", "Old": "Alt", "Open": "Öffnen"}
        result = self.merged(
            {"src/i18n/de.json": catalog(base)}, {"src/i18n/de.json": catalog(ours)}, {"src/i18n/de.json": catalog(theirs)}
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.read("src/i18n/de.json"), catalog({"Back": "Zurück", "Close": "Zumachen", "New": "Neu", "Open": "Öffnen"}))

    def test_baselines_keep_the_lower_ceiling(self) -> None:
        def baseline(files: dict, violations: list[str]) -> dict[str, str]:
            sizes = {"comment": "Never by hand.", "budgets": {"*": 500}, "files": files}
            return {
                ".agents/file-size-baseline.json": json.dumps(sizes, indent=2) + "\n",
                ".agents/architecture-baseline.json": json.dumps({"comment": "x", "violations": violations}, indent=2) + "\n",
            }

        result = self.merged(
            baseline({"src/a.cpp": 600, "src/b.cpp": 700, "src/c.cpp": 800}, ["a includes b", "c includes d"]),
            baseline({"src/a.cpp": 590, "src/b.cpp": 700}, ["c includes d"]),
            baseline({"src/a.cpp": 580, "src/b.cpp": 650, "src/c.cpp": 800}, ["a includes b"]),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expected = baseline({"src/a.cpp": 580, "src/b.cpp": 650}, [])
        for path, text in expected.items():
            self.assertEqual(self.read(path), text)

    def test_architecture_merges_name_by_name(self) -> None:
        ours = ARCHITECTURE.replace('"crypto", "hlc", "log",', '"crypto", "log",').replace(
            '"settings", "theme",', '"settings", "theme", "alpha",'
        )
        ours = ours.replace('modules = ["cli", "mcp"]', 'modules = ["cli", "mcp", "hlc"]')
        theirs = ARCHITECTURE.replace('"settings", "theme",', '"settings", "theme", "beta",')
        theirs = theirs.replace('modules = ["cli", "mcp"]', 'modules = ["cli", "mcp", "gamma"]')
        theirs = theirs.replace('window_only = ["QQuick"]', 'window_only = ["QQuick"]\nallowed_in = ["window"]')
        path = ".agents/architecture.toml"
        result = self.merged({path: ARCHITECTURE}, {path: ours}, {path: theirs})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expected = """
            # The layers.

            [layers.foundation]
            summary = "Settings."
            uses = []
            modules = [
              "crypto", "log",
              "settings", "theme", "alpha",
              "beta",
            ]

            # The front ends.
            [layers.commands]
            summary = "The command line."
            uses = ["foundation"]
            modules = ["cli", "mcp", "hlc", "gamma"]

            [qt]
            window_only = ["QQuick"]
            allowed_in = ["window"]
            """
        self.assertEqual(self.read(path), dedent(expected))

    def test_what_both_sides_changed_their_own_way_is_a_conflict(self) -> None:
        path = ".agents/architecture.toml"
        ours = ARCHITECTURE.replace('summary = "Settings."', 'summary = "Settings and logs."')
        theirs = ARCHITECTURE.replace('summary = "Settings."', 'summary = "Settings and themes."')
        result = self.merged({path: ARCHITECTURE}, {path: ours}, {path: theirs})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("<<<<<<<", self.read(path))
        self.assertIn('summary = "Settings and themes."', self.read(path))


class SuitesFound(Scratch):
    def setUp(self) -> None:
        super().setUp()
        self.cmake = os.environ.get("CMAKE_COMMAND") or shutil.which("cmake")
        if not self.cmake:
            self.skipTest("cmake is not installed")

    def found(self) -> list[str]:
        script = self.root / "find.cmake"
        script.write_text(
            f'include("{REPO}/tests/suites.cmake")\nomabook_test_suites(names "{self.root}/tests")\nmessage(STATUS "suites=${{names}}")\n',
            encoding="utf-8",
        )
        result = self.run_in(self.cmake, "-P", str(script))
        self.assertEqual(result.returncode, 0, result.stderr)
        line = next(line for line in result.stdout.splitlines() if "suites=" in line)
        return [name for name in line.split("suites=", 1)[1].split(";") if name]

    def test_a_new_suite_is_found_by_its_name(self) -> None:
        self.write("tests/shelf_tests.cpp", "")
        self.write("tests/fixtures.cpp", "")
        self.write("tests/fakegh.cpp", "")
        self.assertEqual(self.found(), ["shelf_tests"])
        self.write("tests/atlas_tests.cpp", "")
        self.assertEqual(self.found(), ["atlas_tests", "shelf_tests"])

    def test_the_build_lists_no_suite_by_hand(self) -> None:
        cmakelists = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("omabook_test_suites(", cmakelists)
        self.assertNotIn("foreach(test ", cmakelists)
        sources = {path.stem for path in (REPO / "tests").glob("*_tests.cpp")}
        for extra in (REPO / "tests").glob("*_tests.cmake"):
            self.assertIn(extra.stem, sources, f"{extra.name} has no {extra.stem}.cpp")


if __name__ == "__main__":
    unittest.main()
