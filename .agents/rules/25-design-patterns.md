# Design: patterns that fit, and when not to use them

Prefer the simplest thing that solves today's problem: a function before a
class, a parameter before a pattern. A pattern earns its place when the code
already has the problem it solves. The ones below are already in Omabook; extend
them before inventing another.

| Need | Pattern here | Example | Don't |
|---|---|---|---|
| A family of interchangeable things chosen at run time (catalogs, formats, plugins) | **Strategy** behind a small abstract interface; one subclass per kind | `MetadataSource` (`OpenLibrarySource`, …), `FormatProvider` | add a `switch` on a kind string across the code |
| Steps shared by a family, one step differing | **Template method** only when the shared part is real | `WebMetadataSource::searchUrl`/`parse` | make every method virtual "for later" |
| Many callers, one way to do a thing | **Facade** of free functions in a namespace | `BookOps` | let each front end grow its own copy |
| Tell others something happened | **Observer**: a Qt signal | `Reader::bookChanged`, `GraphFeed::announce` | poll, or have the sender know its listeners |
| Extension points that add themselves | **Registry**: a table filled at start-up | `Formats::addProvider`, MCP tools | put the table in a front end the extensions then depend on (see 10-architecture, known debts) |
| A closed set of alternatives with data | `std::variant` + `std::visit` | | a class hierarchy for three cases |
| A single callback | `std::function` (or a template parameter) | `BookOps::ask(..., piece)` | an interface with one method |

## Dependencies are passed, not fetched

A class gets what it needs through its constructor or a function's parameters
(dependency injection, I.3, R.30). **No new singletons** or `instance()`
accessors; the check refuses them. A dependency that is passed in can be faked in
a test; a global can't. `GraphFeed::instance()` is the one that exists today.

## Interfaces

- **Small and named for the role** (`MetadataSource`, not `IManager`), defined
  by the layer that uses them, not by the one that implements them (DIP).
- Pure virtual interfaces have a virtual destructor and no data (C.121, C.35);
  copying is suppressed to prevent slicing (C.67). QObject does both already.
- Public functions say what they need in their types: a `const Library &` that
  only reads, a `Library &` that writes, a `std::span<const T>`, a `QStringView`.

## One reason to change

A module does one thing (SRP). When a file is at its size budget or a function
over its complexity budget (see 30-size-and-complexity.md), that is usually the
sign that it does two: split by responsibility, and name the new module for
what it does. Duplication of *knowledge* is worth removing; two lines that look
alike but change for different reasons are not.

## Tests

Every behaviour change comes with a test in `tests/` that fails without it,
run offscreen (`bin/test`). Code that is hard to test is usually code that
fetches its dependencies; pass them in instead.
