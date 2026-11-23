<!-- Sync and sharing: https://jellespijker.github.io/omabook/docs/sync.html -->

# Sync and sharing

## Your own computers

Each computer keeps its own whole library. A sync brings two in step over ssh, so anything ssh reaches works, and with Tailscale that is every computer you own. Name the computer once in Settings and press `Y`, or run:

```
omabook sync laptop
omabook sync laptop --dry-run
```

Each side gets the books it lacks with their details, covers and formats. Shelves, stars and finished days travel with every book, and the knowledge graph is merged rather than copied. The first sync between two computers pairs them into one person, so both sign what they write as you.

## Why not a shared folder?

A library is a live database. Two computers writing it through a network filesystem can corrupt it. With a sync, each database is only written by its own computer, a dropped connection leaves both whole, and every computer reads offline.

## Spaces

A space is a body of data with an owner and a say over who may write it. Yours is the personal space. A collection you publish is a space with one writer. A household library or a book club is a shared space with several. Each is signed, and a member removed is refused from then on. (A tenant is something else: the part of the graph a top-level collection keeps to itself.)

```
omabook identity --export
omabook spaces create "Book club" --collection Club
omabook spaces invite "Book club" <person-id> --role reader --out club.omabook-invite
omabook spaces join club.omabook-invite
```

## Publish a collection

`Shift+P` publishes the collection shown; others subscribe with `Shift+O`. They get a space of their own, never merged into their library. What is never shared stays yours: your own notes and what you chose not to project.

## The hub Coming in 0.5

The hub is an Omabook without a window that stays on: it keeps your libraries for your computers, serves phones and e-readers, does background work and takes encrypted snapshots with restic. It is a peer, not a boss: without it, computers sync directly as they do today. It is designed (ADRs 0011 to 0014) and not built yet.
