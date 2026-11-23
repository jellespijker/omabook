<!-- For agents: https://jellespijker.github.io/omabook/docs/agents.html -->

# For agents

Everything the Omabook window does with a library is also a command and a tool, so an AI agent can find, read, fix and organise books without a display. Your books stay on your computer; the agent works on the same library the window has open.

## The command line

Every command takes `--json`, prints errors to standard error, and can work on one library with `--library <folder>`.

```
omabook list "status:reading" --json
omabook search "control theory" --json
omabook show 12 --json
omabook text 12 --section 3
omabook graph find tide --kind concept --json
omabook chronology 12 --upto 5 --json
omabook help agents
```

A search finds books by what you know of them too, but only as far as you have read, so an agent is never ahead of the reader.

## The MCP server

`omabook mcp` runs Omabook as a [Model Context Protocol](https://modelcontextprotocol.io) server on standard input and output. It needs no display. Register it with an agent:

```
omabook mcp install            # Omarchy's default agent
omabook mcp install codex      # or claude, grok, pi, opencode, agy
omabook mcp install --print    # change nothing; show what to add by hand
omabook mcp --tools            # list the tools
```

Any other MCP client takes `{"mcpServers": {"omabook": {"command": "omabook", "args": ["mcp"]}}}`.

### What the tools do

- Find and read: `books_search`, `books_find`, `book_get`, `book_contents`, `book_text`, `book_find`.
- Look after the library: `book_add`, `book_set_details`, `book_set_reading`, `lookup_candidates`, `tidy_propose`, `tidy_apply`, `catalog_export`, `book_convert`.
- Reading and notes: `progress_get`, `bookmarks_list`, `annotations_list`, `annotations_export`.
- Knowledge graph and timelines: `graph_find`, `graph_node`, `graph_book`, `graph_build`, `graph_export`, `chronology`.
- Ask Omabook's own AI: `ask_book`, `ask_library`, `suggest_reading`.
- Sync, plugins and look: `sync_run`, `plugins_list`, `plugin_add`, `customize_set`.

Every tool says in its annotations whether it only reads, changes something or takes something away, so a client can ask before it acts. `omabook mcp --tools` lists what your build has.

## A skill for coding agents

`omabook skill install` teaches your coding agents the commands, so they can use the command line without the MCP server.

## The interface is files

Keys, colors, views and panels are files in `~/.config/omabook/`, followed while the app runs. `omabook customize` prints the folder and writes a guide there for agents to follow. Changes made through `customize_set` get the same checks as a file edited by hand and can be undone.

## What stays yours

Omabook asks no account. An agent reads your books through your own computer, and what reaches any AI is chosen in [the AI settings](ai.html), capability by capability.
