#pragma once

#include <QStringList>

// The `hub` command (#179): `hub run`, the daemon that keeps the hub's
// libraries as their one writer (ADR 0013), started by its systemd unit;
// `hub session`, what a computer's key runs on the hub (ADR 0012), handing
// each request it reads to the daemon and printing the answer; and `hub
// status`, the daemon's status asked from the hub itself.
int runHubCli(const QStringList &arguments);
