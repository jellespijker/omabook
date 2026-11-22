#!/usr/bin/env python3
"""Each harness check fires on what it is for, and stays quiet on old code.

Every test works in a scratch git repository holding a copy of the harness, so
the checks see a real diff. Run: python3 .agents/hooks/test_hooks.py
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
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
HARNESS = [".agents", "rules", "rule-tests", "sgconfig.yml", ".clang-format", "ruff.toml", "bin/jobs", "bin/release-notes"]


def isolated() -> dict[str, str]:
    """This process's environment without git's own variables.

    Run as a hook, these tests inherit GIT_DIR, GIT_INDEX_FILE and the rest from
    the commit being made, and every git command would work on that repository
    instead of the scratch one: commit into it, write its config, re-init it.
    """
    return {name: value for name, value in os.environ.items() if not name.startswith("GIT_")}


class Scratch:
    """A repository with the harness and one committed source file per test."""

    def __init__(self) -> None:
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        for item in HARNESS:
            source, target = REPO / item, self.root / item
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, target, ignore=shutil.ignore_patterns("__pycache__")) if source.is_dir() else shutil.copy2(
                source, target
            )
        (self.root / "src").mkdir()
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "harness@example.invalid")
        self.git("config", "user.name", "Harness test")
        self.commit()

    def git(self, *args: str) -> None:
        subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True, env=isolated())

    def write(self, path: str, text: str) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(textwrap.dedent(text).lstrip(), encoding="utf-8")

    def commit(self) -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "Start (#1)")

    def run(self, check: str, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(self.root / ".agents/hooks" / check), *args],
            cwd=self.root,
            input=stdin,
            capture_output=True,
            text=True,
            env=isolated(),
        )

    def close(self) -> None:
        self.folder.cleanup()


class HarnessTest(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Scratch()

    def tearDown(self) -> None:
        self.repo.close()

    def assertFires(self, result: subprocess.CompletedProcess, text: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertIn(text, result.stderr)

    def assertQuiet(self, result: subprocess.CompletedProcess) -> None:
        self.assertEqual(result.returncode, 0, result.stderr)


class Isolation(unittest.TestCase):
    def test_a_hooks_git_variables_leave_that_repository_alone(self) -> None:
        with tempfile.TemporaryDirectory() as other:
            subprocess.run(["git", "init", "-q", other], check=True, env=isolated())
            config = (Path(other) / ".git/config").read_text(encoding="utf-8")
            hook = {"GIT_DIR": f"{other}/.git", "GIT_INDEX_FILE": f"{other}/.git/index", "GIT_WORK_TREE": other}
            with mock.patch.dict(os.environ, hook):
                repo = Scratch()
                repo.write("lib/a.js" if (REPO / "main.js").exists() else "src/a.cpp", "x\n")
                repo.commit()
                repo.close()
            self.assertEqual((Path(other) / ".git/config").read_text(encoding="utf-8"), config)
            log = subprocess.run(["git", "-C", other, "log", "--oneline"], capture_output=True, text=True, env=isolated())
            self.assertEqual(log.stdout, "", "a commit went into the repository the hook ran for")


class PretoolGuard(HarnessTest):
    def guard(self, tool: str, **args: str) -> subprocess.CompletedProcess:
        return self.repo.run("pretool_guard.py", stdin=json.dumps({"tool_name": tool, "tool_input": args}))

    def test_commit_must_name_an_issue(self) -> None:
        self.assertFires(self.guard("Bash", command='git commit -m "Tidy the shelf"'), "names no GitHub issue")
        self.assertQuiet(self.guard("Bash", command='git commit -m "Tidy the shelf (#29)"'))
        self.assertQuiet(self.guard("Bash", command='git commit -m "Tidy it, for jellespijker/omabook-storyteller#2"'))

    def test_hooks_are_not_skipped(self) -> None:
        self.assertFires(self.guard("Bash", command='git commit --no-verify -m "x (#1)"'), "skips the hooks")
        self.assertQuiet(self.guard("Bash", command='git commit -m "Add the -name flag (#1)"'))

    def test_builds_are_limited(self) -> None:
        self.assertFires(self.guard("Bash", command="cmake --build build -j$(nproc)"), "out of memory")
        self.assertFires(self.guard("Bash", command="make -j"), "out of memory")
        self.assertQuiet(self.guard("Bash", command='cmake --build build -j"$(bin/jobs)"'))

    def test_secrets_and_home_paths_are_not_written(self) -> None:
        token = "ghp_" + "a1" * 20
        self.assertFires(self.guard("Write", file_path="src/a.cpp", content=f'auto t = "{token}";'), "GitHub token")
        self.assertFires(self.guard("Edit", file_path="src/a.cpp", new_string='auto p = "/home/someone/Books";'), "home folder")
        self.assertQuiet(self.guard("Edit", file_path="src/a.cpp", new_string="auto p = QDir::homePath();"))

    def test_unreadable_payload_lets_the_call_through(self) -> None:
        self.assertQuiet(self.repo.run("pretool_guard.py", stdin="not json"))


class CommitMessage(HarnessTest):
    def message(self, text: str) -> subprocess.CompletedProcess:
        (self.repo.root / "MSG").write_text(text, encoding="utf-8")
        return self.repo.run("check_commit_msg.py", "MSG")

    def test_names_an_issue(self) -> None:
        self.assertFires(self.message("Tidy the shelf\n"), "names no GitHub issue")
        self.assertQuiet(self.message("Tidy the shelf\n\nPart of #29.\n"))
        self.assertQuiet(self.message("Merge branch 'kanban/issue-29'\n"))


class AstGrep(HarnessTest):
    def test_new_lines_are_checked_and_old_ones_are_not(self) -> None:
        self.repo.write("src/old.cpp", "void drop(int *p) { delete p; }\n")
        self.repo.commit()
        self.repo.write("src/old.cpp", "void drop(int *p) { delete p; }\nint *none() { return nullptr; }\n")
        self.assertQuiet(self.repo.run("check_ast_grep.py", "--changed", "src/old.cpp"))
        self.repo.write("src/old.cpp", "void drop(int *p) { delete p; }\nint *none() { return NULL; }\n")
        self.assertFires(self.repo.run("check_ast_grep.py", "--changed", "src/old.cpp"), "no-null-macro")


class Format(HarnessTest):
    def test_changed_lines_of_an_old_file_are_a_suggestion(self) -> None:
        self.repo.write("src/old.cpp", "int answer() { return 42; }\n")
        self.repo.commit()
        self.repo.write("src/old.cpp", "int answer() { return 42; }\nint  other( ) {return 1;}\n")
        result = self.repo.run("check_format.py", "--changed", "src/old.cpp")
        self.assertQuiet(result)
        self.assertIn("a suggestion", result.stderr)

    def test_a_new_file_is_formatted(self) -> None:
        self.repo.write("src/fresh.cpp", "int  answer( ) {return 42;}\n")
        self.assertFires(self.repo.run("check_format.py", "--changed", "src/fresh.cpp"), "not formatted")
        self.repo.write("src/fresh.cpp", "int answer() { return 42; }\n")
        self.assertQuiet(self.repo.run("check_format.py", "--changed", "src/fresh.cpp"))


class Architecture(HarnessTest):
    def setUp(self) -> None:
        super().setUp()
        (self.repo.root / ".agents/architecture-baseline.json").write_text('{"violations": []}\n')

    def test_an_include_may_not_point_up(self) -> None:
        self.repo.write("src/log.cpp", '#include "library.h"\n')
        self.assertFires(self.repo.run("check_architecture.py", "--changed", "src/log.cpp"), "foundation includes library.h")
        self.repo.write("src/log.cpp", '#include "settings.h"\n')
        self.assertQuiet(self.repo.run("check_architecture.py", "--changed", "src/log.cpp"))

    def test_qt_quick_belongs_to_the_window(self) -> None:
        self.repo.write("src/library.cpp", "#include <QQuickItem>\n")
        self.assertFires(self.repo.run("check_architecture.py", "--changed", "src/library.cpp"), "only the window may")

    def test_every_module_is_placed(self) -> None:
        self.repo.write("src/stray.cpp", "int x = 1;\n")
        self.assertFires(self.repo.run("check_architecture.py", "--changed", "src/stray.cpp"), "unplaced")


class Wording(HarnessTest):
    def test_a_sentence_below_the_front_ends_is_a_finding(self) -> None:
        self.repo.write("src/formats.cpp", "int open() {\n    return 0;\n}\n")
        self.repo.commit()
        self.repo.write("src/formats.cpp", 'auto open() {\n    return std::unexpected(\n        "There is no such file: " + path);\n}\n')
        self.assertFires(self.repo.run("check_wording.py", "--changed", "src/formats.cpp"), "English sentence")
        self.repo.write("src/formats.cpp", "auto open() {\n    return std::unexpected(Failure(Failure::Code::NoSuchFile, {path}));\n}\n")
        self.assertQuiet(self.repo.run("check_wording.py", "--changed", "src/formats.cpp"))

    def test_the_front_ends_and_old_lines_are_left_alone(self) -> None:
        self.repo.write("src/reader.cpp", 'void f() { fail(std::unexpected("This book is gone.")); }\n')
        self.assertQuiet(self.repo.run("check_wording.py", "--changed", "src/reader.cpp"))
        self.repo.write("src/library.cpp", 'auto a() { return std::unexpected("A book needs a title."); }\nint b() {\n    return 1;\n}\n')
        self.repo.commit()
        self.repo.write("src/library.cpp", 'auto a() { return std::unexpected("A book needs a title."); }\nint b() {\n    return 2;\n}\n')
        self.assertQuiet(self.repo.run("check_wording.py", "--changed", "src/library.cpp"))


class FileSize(HarnessTest):
    def setUp(self) -> None:
        super().setUp()
        baseline = {"budgets": {"tests/*": 1000, "*": 500}, "files": {"src/big.cpp": 600}}
        (self.repo.root / ".agents/file-size-baseline.json").write_text(json.dumps(baseline))

    def test_grandfathered_files_do_not_grow(self) -> None:
        self.repo.write("src/big.cpp", "int x;\n" * 600)
        self.repo.commit()
        self.repo.write("src/big.cpp", "int x;\n" * 601)
        self.assertFires(self.repo.run("check_file_size.py", "--changed", "src/big.cpp"), "may not grow")

    def test_new_files_meet_the_budget(self) -> None:
        self.repo.write("src/new.cpp", "int x;\n" * 501)
        self.assertFires(self.repo.run("check_file_size.py", "--changed", "src/new.cpp"), "over its ceiling of 500")


class Complexity(HarnessTest):
    BRANCHY = "int pick(int v) {\n" + "".join(f"    if (v == {i}) return {i};\n" for i in range(20)) + "    return -1;\n}\n"

    def test_a_new_complex_function_fails(self) -> None:
        self.repo.write("src/pick.cpp", self.BRANCHY)
        self.assertFires(self.repo.run("check_complexity.py", "--changed", "src/pick.cpp"), "cyclomatic complexity")

    def test_old_complexity_that_does_not_grow_passes(self) -> None:
        self.repo.write("src/pick.cpp", self.BRANCHY)
        self.repo.commit()
        self.repo.write("src/pick.cpp", self.BRANCHY + "int other() { return 1; }\n")
        self.assertQuiet(self.repo.run("check_complexity.py", "--changed", "src/pick.cpp"))


class Changelog(HarnessTest):
    FRAGMENT = "---\nsection: Fixed\n---\n\n- The shelf sorts again (#7).\n"

    def setUp(self) -> None:
        super().setUp()
        self.repo.write("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n\n## [0.3.0] - 2026-11-22\n")
        self.repo.write("CMakeLists.txt", "project(omabook VERSION 0.3.0 LANGUAGES CXX)\n")
        self.repo.write("src/shelf.cpp", "int a = 1;\n")
        self.repo.commit()

    def check(self, *args: str) -> subprocess.CompletedProcess:
        return self.repo.run("check_changelog.py", *(args or ("--changed",)))

    def test_the_changelog_is_edited_only_by_a_release(self) -> None:
        self.repo.write("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n\n- A line (#7).\n\n## [0.3.0] - 2026-11-22\n")
        self.assertFires(self.check(), "lines added under [Unreleased]")
        self.repo.write("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n\n## [0.3.0] - 2026-11-21\n")
        self.assertFires(self.check(), "a released section changed outside a release")
        self.repo.write("CMakeLists.txt", "project(omabook VERSION 0.4.0 LANGUAGES CXX)\n")
        self.assertQuiet(self.check())

    def test_lines_may_leave_unreleased_for_fragments(self) -> None:
        self.repo.write("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n\n### Fixed\n\n- Old (#6).\n\n## [0.3.0] - 2026-11-22\n")
        self.repo.commit()
        self.repo.write("CHANGELOG.md", "# The change log\n\n## [Unreleased]\n\n## [0.3.0] - 2026-11-22\n")
        self.repo.write("changes/6.md", self.FRAGMENT.replace("#7", "#6"))
        self.assertQuiet(self.check())

    def test_a_commit_named_release_may_edit_it(self) -> None:
        self.repo.git("checkout", "-q", "-b", "release")
        self.repo.write("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n\n## [0.3.1] - 2026-11-23\n\n## [0.3.0] - 2026-11-22\n")
        self.repo.git("commit", "-q", "--no-verify", "-am", "Fix a typo in the notes (#7)")
        self.assertFires(self.check("--base", "main"), "a released section changed")
        self.repo.git("commit", "-q", "--no-verify", "--amend", "-m", "Release 0.3.1: the shelf sorts again (#7)")
        self.assertQuiet(self.check("--base", "main"))

    def test_a_change_to_what_omabook_does_leaves_a_fragment(self) -> None:
        self.repo.write("src/shelf.cpp", "int a = 2;\n")
        self.assertFires(self.check(), "no changes/<issue>.md says how")
        self.repo.write("changes/7.md", self.FRAGMENT)
        self.assertQuiet(self.check())
        self.repo.write("changes/7.md", self.FRAGMENT.replace("Fixed", "Mended"))
        self.assertFires(self.check(), "section is Mended")
        self.repo.write("changes/7.md", "---\nsection: none\n---\n\n- Only the code moved.\n")
        self.assertQuiet(self.check())

    def test_a_change_beside_the_program_needs_none(self) -> None:
        self.repo.write("docs/notes.md", "How it works.\n")
        self.assertQuiet(self.check())


if __name__ == "__main__":
    unittest.main()
