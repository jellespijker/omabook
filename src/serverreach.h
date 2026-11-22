#pragma once

#include <QString>
#include <chrono>
#include <optional>

// Whether a server can be reached at all (#174): a connection to its host and
// port, tried for a moment, never what it answers. A server that answers with
// an error was reached; only one that can't be (off, the laptop away from
// home) is "away", and a capability with a fallback then goes there.
//
// What a try came to is held for a while, so a server that is away costs one
// short wait, not one for every request, and nothing hangs; once that has
// passed it is tried again, so the first is gone back to by itself as soon as
// it answers. Safe to ask from any thread.
namespace ServerReach {

// How long a connection is tried for, at most.
inline constexpr std::chrono::milliseconds tryFor{1000};
// How long what a try came to is held: a server found there, and one found away.
inline constexpr std::chrono::seconds heldThere{10};
inline constexpr std::chrono::seconds heldAway{30};

// Whether a connection to the server at this address (".../v1", or any URL)
// can be made now, by what is held, else by trying.
[[nodiscard]] bool reachable(const QString &baseUrl);
// What was last found, while it is held; none when nothing is.
[[nodiscard]] std::optional<bool> known(const QString &baseUrl);
// Forgets what was found for this address, or for all with none: a row's
// Test asks again, as a test of going back does.
void forget(const QString &baseUrl = {});
// How long findings are held, both ways, in place of heldThere and heldAway:
// for a test of going back to the first by itself. Zero or less restores them.
void holdFor(std::chrono::milliseconds duration);

} // namespace ServerReach
