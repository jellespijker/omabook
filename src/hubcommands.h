#pragma once

#include <QStringList>

// The `hub` command (#179): `hub run`, the daemon that keeps the hub's
// libraries as their one writer (ADR 0013), started by its systemd unit;
// `hub session`, what a computer's key runs on the hub (ADR 0012): with
// --key, the session's protocol for that computer (HubSession, #180), and
// without, on the hub by its own user, each request handed to the daemon; `hub
// status`, the daemon's status, asked on the hub or of the hub at a host; and
// the links between a person's computers and their hub (hublinkcommands.h).
int runHubCli(const QStringList &arguments);
