#!/usr/bin/env python3
"""Each harness check fires on what it is for, and stays quiet on old code.

Every test works in a scratch git repository holding a copy of the harness, so
the checks see a real diff. Run: python3 .agents/hooks/test_hooks.py
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HARNESS = [".agents", "rules", "rule-tests", "sgconfig.yml", ".clang-format", "ruff.toml", "bin/jobs"]


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
        subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True)

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


if __name__ == "__main__":
    unittest.main()
