# File size and function complexity

A big file is expensive for every person and agent that reads it afterwards;
a complex function is where the bugs are. Both have a budget.

| Measure | Budget | Check |
|---|---|---|
| Lines in a source file (`.cpp`, `.h`, `.qml`, `.js`, `.py`, `.sh`) | 500 | `check_file_size.py` |
| Lines in a test file (`tests/`) | 1000 | `check_file_size.py` |
| Cyclomatic complexity of a function | 15 | `check_complexity.py` (lizard) |
| Lines of code in a function | 80 | `check_complexity.py` |
| Parameters of a function | 6 | `check_complexity.py` |

## Old code

Files that were over budget when the check came in are listed in
`.agents/file-size-baseline.json` at their size then. **They may shrink, never
grow**; when one shrinks the commit lowers its ceiling, and once it is within
budget it leaves the list. A new file is never listed.

A function that was over a limit before is checked against itself: it fails
only if the change makes it worse on that measure. Touching it is fine;
growing it is not.

## Plan for it

Before changing a file, look at its headroom:

```bash
python3 .agents/hooks/check_file_size.py --report src/cli.cpp src/mcp.cpp
python3 .agents/hooks/check_complexity.py --report src/cli.cpp
```

If the change doesn't fit, the plan says **which responsibility moves out,
where to, and what the new module is called**, and the new module is placed in
`.agents/architecture.toml`. A file at its ceiling is a reason for a new
module, never for an exception. For `tests/`, a new area gets a new
`tests/<area>_tests.cpp`, which the build finds by its name (`tests/suites.cmake`).

## Meet the budget honestly

These pass the numbers and fail the rule:

- deleting blank lines or comments, joining statements onto one line
- shortening names, inlining helpers so a function grows elsewhere
- moving code to a file named `*_part2.cpp` or `utils.cpp` with no single purpose

Split by responsibility: a command's argument parsing from what it does, a
format's parsing from its rendering, one big `switch` into a table or a
function per case.
