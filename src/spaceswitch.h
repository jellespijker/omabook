#pragma once

#include <QString>
#include <expected>

#include "failure.h"
#include "spaces.h"

class Library;

// Switching a part of a space on or off once it is there (SpaceParts, #85),
// the same for the command line, the MCP server and the window: in a shared
// space as any of its writers, by an operation of the space (entity "space",
// field "part:<name>", the latest by clock winning), so it goes to every
// member; in a space published here as its owner, whose log then follows
// (PublishedSpace::follow), and a part switched off is taken back from those
// who had it. A space one only subscribes to is its owner's to switch.
namespace SpaceSwitch {

// The space `which` names (its id or its name), of those this library shares or publishes.
[[nodiscard]] std::expected<Spaces::Recorded, Failure> resolve(const Library &library, const QString &which);

struct Switched {
    QString space;
    QString kind; // "shared" or "published"
    QString name;
    QString part;
    bool on = false;
    bool changed = false; // it was not so already
};

// Whether the part is on in the space: a shared space's as its writers last
// said, one published here as its owner chose, one subscribed to as its
// latest snapshot says.
[[nodiscard]] bool isOn(const Library &library, const QString &space, const QString &part);

// Switches the part `part` of the space `which` on or off. Of the parts, only
// those the registry says are switched (SpaceParts::Part::switched).
[[nodiscard]] std::expected<Switched, Failure> set(const Library &library, const QString &which, const QString &part, bool on);

} // namespace SpaceSwitch
