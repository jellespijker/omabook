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
- **How it is built (#180).** A computer's key for its hubs is its own, ed25519, made by Omabook. `hub add` installs it
  over the person's ssh (`omabook hub allow` there), signed by their identity, as
  `restrict,command="'<omabook>' hub session --key '<its fingerprint>'"`: the hub writes the fingerprint, so the
  session knows the computer by its key, never by what the client says, and `restrict` stands for
  `no-port-forwarding,no-pty` and the rest. The session speaks a versioned protocol, one JSON object a line: first
  `{"request": "hello", "protocol": 1}`, then only its requests; anything else ends it. The requests the daemon
  answers are handed on with the computer's name in them, the ground the sync with the hub (#188) builds on.

- **The owner under another id (#283).** Pairing gives the computers of one person one id (the smaller wins, ADR
  0001), so an owner's id changes after the first computer was linked. The hub keeps every id its owner has gone by
  (`owner_ids` in `computers.json`, beside `owner`). It adds one only when told by a computer that signs with the new
  key and with a key the hub already knows as its owner's (a computer keeps the keys it gave up), in a message naming
  the new id and that computer's key for the hub (`HubOwner`): with `hub allow --former` at a link the hub would
  refuse, or in the hello of a linked computer, said again after a sync that paired it. A computer with none of the
  owner's keys is someone else, as before: nobody is taken for the owner because they could reach `hub allow`, and a
  computer that never paired with one of the owner's is paired first. Old hubs and old computers go on as before;
  neither side sends the other what it can't read except a second `hub allow`, whose refusal changes nothing.

## Alternatives considered

- **An HTTPS API signed with the identity key**: workable, but it doubles the transport and the attack
  surface for no gain while sync needs ssh anyway.
