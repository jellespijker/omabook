# AGENTS.md — Omabook

Omabook is developed in the [omabook-workspace](https://github.com/jellespijker/omabook-workspace),
the main workspace for Omabook and its plugins (it holds the shared Kanban board,
labels and milestones). Work from there: `omabook-workspace/omabook/`, or the
worktree `herdr-kanban claim` gives you. Read the workspace's `AGENTS.md` first.

C++23 on Qt 6 (Quick, QML), built with CMake. `src/` is one folder of modules
in layers; QML views sit beside the C++ in `src/`.

## The harness

Every edit and commit is checked: formatting, modern C++23 (clang-tidy and the
ast-grep rules in `rules/cpp/`), the architecture's layers, file-size and
complexity budgets, secrets, and an issue number in the commit message. Only
what a change adds is judged. **Read the rule that applies before you write
the code**, not after a check fails:

| Rule | When it applies |
|---|---|
| [.agents/rules/00-harness.md](.agents/rules/00-harness.md) | What runs when, and what to do when a check is wrong |
| [.agents/rules/10-architecture.md](.agents/rules/10-architecture.md) | Adding a file, or an `#include` |
| [.agents/rules/20-modern-cpp.md](.agents/rules/20-modern-cpp.md) | Any C++ |
| [.agents/rules/25-design-patterns.md](.agents/rules/25-design-patterns.md) | A new class, interface or dependency |
| [.agents/rules/30-size-and-complexity.md](.agents/rules/30-size-and-complexity.md) | Growing a file or a function |

Once per clone: `bin/setup` (the harness's hooks and the merge drivers). Before
handing work in: `bin/check`.
Never skip the hooks (`--no-verify`) and never edit a baseline by hand.

## Building

Build with `bin/build` (or `bin/sanitize`), or pass `-j"$(bin/jobs)"` to
`cmake --build`. Never use a bare `-j` or `-j$(nproc)`: with the Makefile
generator a bare `-j` is unlimited, and on a low-memory machine the compile
thrashes until systemd-oomd kills the whole terminal session, agents included.
Run one build at a time; do not build several `build-*` directories in parallel.

## Testing

`bin/test` builds and runs every test suite offscreen. A behaviour change comes
with a test that fails without it. `bin/sanitize` runs them under AddressSanitizer
and UBSan; run it after touching anything that holds a pointer. A ticket lands
only after `bin/build` and every test suite pass.

## Commits

Every commit message contains the number of the GitHub issue it belongs to, in
the subject or the body: `Turn whole pages, or scroll on through the chapters (#26)`.
Write `#12` for an issue of this repository and `jellespijker/omabook-storyteller#2`
for another's. If there is no issue for the work, deposit one
(`herdr-kanban deposit`) before you commit. A change someone using Omabook would
notice gets a line in its change notes (below), and in `src/whatsnew.cpp` when
it is worth showing in the window.

## Change notes, tests and the shared files

Tickets land side by side, so a change adds files of its own rather than lines
to shared lists (#198):

- **Change notes** go in `changes/<issue>.md`, never in `CHANGELOG.md`: front
  matter `section: Added` (or Changed, Deprecated, Removed, Fixed, Security),
  then the line or lines, each starting with `- `. A change to `src/` or `share/`
  no one would notice says so with `section: none` and why. `bin/check`
  refuses one without, and any line added to `CHANGELOG.md`; a release runs
  `bin/release-notes <version>`, which writes the version's section and removes
  the fragments.
- **Test suites are found by name**: a new `tests/<area>_tests.cpp` is built and
  run with no list to edit. What it needs beyond the others goes in
  `tests/<area>_tests.cmake` (see `tests/suites.cmake`).
- **What stays shared** (the translation catalogs, `.agents/architecture.toml`,
  the baselines) is merged by the drivers in `bin/merge/`, which `bin/setup`
  registers. After a merge that touched strings, run `bin/i18n`.

## Tickets

Issues and milestones for this repository are on the workspace board (alias
`omabook`), with `type:*` and `domain:*` labels. See the workspace `AGENTS.md`.
