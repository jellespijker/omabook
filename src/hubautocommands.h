#pragma once

#include <QStringList>

// Automatic sync with a hub (#191, HubAutoSync), as `omabook hub sync` takes
// it: `--auto on|off` switches it on or off for the hub named (the one this
// computer is linked to when it names none), `--background on|off` allows the
// background service to sync with the window closed, from login on, or takes
// it away, and `--service` is what that service runs. `given` is `omabook
// sync`'s arguments, as runHubSync makes them.
[[nodiscard]] bool isHubAutoCommand(const QStringList &given);
// Whether it names a hub: only `--auto` does.
[[nodiscard]] bool hubAutoNamesAHub(const QStringList &given);
int runHubAuto(const QStringList &given);
