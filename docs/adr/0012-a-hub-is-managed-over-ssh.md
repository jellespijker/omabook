# 12. A hub is managed over ssh, through a forced command

- Status: accepted (#178)
- Date: 2026-11-22

## Context

A person's computers manage their hub: they link to it, sync, take and restore snapshots, read its logs and
change its AI settings. Sync already runs over ssh with the person's keys. A hub could instead offer an API of
its own over HTTPS.

## Decision

- **Everything a computer asks of its hub goes over ssh**, with the keys sync already uses.
- **A computer's key opens Omabook, never a shell.** It is installed in the hub's `authorized_keys` with
  `command="omabook hub session"` (and `no-port-forwarding,no-pty`). `hub session` takes a fixed set of
  requests and hands them to the daemon (ADR 0013).
- **Only the person's own computers manage a hub.** A session states the identity (`omabook identity`); a hub
  answers only its owner's.
- **The only ports a hub opens** are OPDS and KOReader's progress sync, on the tailnet (#177, #193).
- **In a container** (#195): a small sshd that allows only the forced command, or Tailscale SSH.

## Consequences

- There's no second protocol to design, version and secure, and no new open port for management.
- Being reachable means being reachable over ssh: on the tailnet, or as the person sets up ssh.
- The window's Hubs panel (#189) is a client of the same session requests as the command line.

## Alternatives considered

- **An HTTPS API signed with the identity key**: workable, but it doubles the transport and the attack
  surface for no gain while sync needs ssh anyway.
