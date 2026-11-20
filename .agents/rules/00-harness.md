# The harness: what checks a change, and when

Every change to Omabook is checked the same way, whoever makes it. The checks
look only at what a change **adds**: old code that predates a rule is not a
finding. They live in `.agents/hooks/` and run at three moments:

| When | What runs | Wired in |
|---|---|---|
| Before an agent's tool call | `pretool_guard.py`: no secrets or home-folder paths written, no commit without an issue number, no `--no-verify`, no unlimited `-j`, no force-push of `main` | `.claude/settings.json` |
| After an agent edits a file | `post_edit_check.py`: format, structural rules, architecture, wording, ruff, size, complexity of that file | `.claude/settings.json` |
| On `git commit` | all of the above on what is staged, plus clang-tidy, Talisman and the commit message | `.pre-commit-config.yaml` |
| In CI | the same, against the merge base with `main` | `.github/workflows/harness.yml` |

Set up a clone once: `uvx pre-commit install`. Run everything on your change by
hand: `bin/check` (it compares the working tree with `main`).

## The checks

| Check | Rule file | Tool |
|---|---|---|
| Formatting: new files whole; changed lines in old files as a suggestion | `.clang-format` | clang-format |
| Modern C++23 and the guidelines | [20-modern-cpp.md](20-modern-cpp.md), `.clang-tidy` | clang-tidy (needs `bin/build`) |
| Project rules the compiler can't see | [20-modern-cpp.md](20-modern-cpp.md), `rules/cpp/*.yml` | ast-grep |
| Layers and dependencies | [10-architecture.md](10-architecture.md), `.agents/architecture.toml` | `check_architecture.py` |
| Failures below the front ends are codes, not English sentences | [20-modern-cpp.md](20-modern-cpp.md), ADR 0005 | `check_wording.py` |
| Design | [25-design-patterns.md](25-design-patterns.md) | review, plus the rules above |
| Python (the harness, the Calibre bridge) | `ruff.toml` | ruff |
| File size, function complexity | [30-size-and-complexity.md](30-size-and-complexity.md) | `check_file_size.py`, lizard |
| Secrets | `.talismanrc` | Talisman, `pretool_guard.py` |
| Commit names its issue | the workspace's `AGENTS.md` | `check_commit_msg.py` |

## When a check is wrong

- **A finding you disagree with in one place**: suppress it there and say why,
  on the line: `// NOLINT(check-name): the reason` for clang-tidy,
  `// ast-grep-ignore: rule-id` for ast-grep, `# noqa: CODE` for ruff. A suppression without a reason is
  a finding in review.
- **A rule that is wrong everywhere**: change the rule (`.clang-tidy`,
  `rules/cpp/`), with its reason in the file, in a commit of its own.
- **Never** edit a baseline by hand to let a change through
  (`.agents/architecture-baseline.json`, `.agents/file-size-baseline.json`).
  They only ever shrink; the checks shrink them for you.
- **Never** skip the hooks (`--no-verify`). The guard refuses it.

## Changing the harness

Every hook has a test in `.agents/hooks/test_hooks.py`, every structural rule
has cases in `rule-tests/`. Both run when the harness changes:
`python3 .agents/hooks/test_hooks.py` and `ast-grep test --skip-snapshot-tests`.
