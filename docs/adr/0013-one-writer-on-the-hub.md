# 13. One writer on the hub

- Status: accepted (#178)
- Date: 2026-11-22
- Builds on: #161 (a batch of writes locks the library from its start)

## Context

A hub's library is written by many at once: syncs from several computers, background work (the knowledge
graph, the meaning index, writing out), plugins' background hooks, and snapshots, which must see a
consistent library. Each `ssh hub omabook …` would start a process of its own that opens the SQLite file.
SQLite serialises writers only by refusing or waiting on its lock, and a write that reads first and writes
later fails at once when another wrote between (#161 found this as "database is locked").

## Decision

- **On a hub, only the hub daemon (`omabook hub run`, #179) opens a library for writing.**
- **Everything else asks it**, over a local Unix socket: `omabook hub session` for a computer, the job
  runner for background work, plugins through their usual API.
- **The daemon serialises the writes**, and takes snapshots inside the same lock (ADR 0014).
- An architecture rule keeps it so: in hub mode, only the daemon's module opens a library for writing.

## Consequences

- A small request layer between the session command and the daemon, and a daemon that must keep running
  (a systemd unit that restarts it).
- No lock fights or torn reads on a hub, and a snapshot always sees a whole library.
- On a person's computer, nothing changes: the window is its one writer, as today.
- Opening a library may write (bringing it up to date, the change log), so on a hub another program is refused
  the libraries the daemon keeps altogether, reads too, for as long as it runs (#179: a hold in each library's
  folder, `.omabook-hub.lock`). What it needs to read, it asks the daemon too.
- The session runs as the daemon's user, as its socket is that user's alone: with the system unit, a computer's
  key is installed for the hub's own user (`omabook`).

## Alternatives considered

- **Every process writes, SQLite's lock sorts it out**: the failure #161 met, multiplied by every computer and job.
