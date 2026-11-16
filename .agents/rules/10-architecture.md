# Architecture: the layers of src/

`src/` is one folder, but not one heap: every module (a file stem, `library`
for `library.h` and `library.cpp`) belongs to a layer, and a layer depends only
on the layers below it. The map is `.agents/architecture.toml`; the check is
`.agents/hooks/check_architecture.py`.

```
  app          main()
  commands  |  window          the two front ends; peers, neither includes the other
  operations                   what both front ends do, written once (BookOps, collections, sync, the tools)
  services                     AI, metadata sources, reading sites, Calibre, plugins, the help's content
  library                      the library and the knowledge graph
  formats                      opening and reading book files
  foundation                   settings, logging, theme, interface files, text cleaning
```

Two more rules:

- **Qt Quick and QML stay in the window.** `<QQuick…>` and `<QQml…>` are
  included only by `window` and `app`. Everything below works without a window:
  the command line and the MCP server run it headless, and the tests can test
  it without one. `QJSEngine` (script plugins) is not QML and is fine anywhere.
- **Every new module is placed** in `.agents/architecture.toml` in the same
  change that adds it. Placing it is the design decision: say in the commit
  why it belongs there.

## Where new code goes

Ask what the code needs, not who calls it. Code that parses a format goes in
`formats` even if only the reader uses it today; a rule about books goes in
`library` even if only the command line asks. The front ends stay thin: a
command or a window action reads its input, calls `operations` or below, and
shows the result. Something both the command line and the window do belongs in
`operations` (see `bookops.h`): written once, tested once.

## Resolving a violation

When code low down needs something from higher up, the dependency is pointing
the wrong way. In order of preference:

1. **Move the code down.** Often the needed piece (a type, a pure function)
   doesn't really belong to the higher module. Move it to the lowest layer that
   can hold it.
2. **Pass it in.** The lower module takes a parameter, a `std::function`, or a
   small interface it defines itself; the higher layer supplies it. (Dependency
   inversion; DIP.)
3. **Raise the module.** If it really is about the higher layer's concerns,
   it belongs there; move it up in `architecture.toml`.

Never "fix" a violation by forward-declaring the higher type to dodge the
include: the dependency is still there.

## Known debts (the baseline)

`.agents/architecture-baseline.json` lists the includes that broke these
rules when the check came in. They may only disappear. The main one:

- **Tools register themselves with the MCP server.** `graphtools`, `plugintools`,
  `scripttools`, `pluginsources` and `pluginfinder` include `mcp.h` for
  `Mcp::Tool` and `Mcp::registerTool`. The tool *registry* (the `Tool` type,
  the schema helpers and the table of tools) is not the MCP *server*: it
  belongs in `operations` or `services`, and `mcp.cpp` should read the table. Moving it
  clears five entries.
- `graphcontext` (library) uses `Prompts::graphKinds()` (services): the list of
  graph kinds is data about the graph and can move into `graph`.
- `scriptplugin` and `pluginsources.h` reach up into `operations`; script
  plugins call the tools through the registry above once it has moved.

After fixing one, run `python3 .agents/hooks/check_architecture.py --update`
and commit the smaller baseline.

## Decisions

Decisions that shape more than one module are recorded in `docs/adr/`, numbered, with their context,
what was decided, the alternatives and what follows from it. Read the ones that touch your change
before you start; write one when your change decides something the next person would otherwise ask.

- [0001: Owned spaces, replicated as change logs, in place of state sync](../../docs/adr/0001-owned-spaces-and-replicated-logs.md)
