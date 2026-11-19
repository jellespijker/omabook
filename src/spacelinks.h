#pragma once

#include <QList>
#include <QString>
#include <optional>

class Library;

// Where a space travels by a transport (ADR 0001, *Transports are adapters*;
// #7), as this library knows it: the place a space published here was last
// sent to, so that sending it again updates that place rather than making
// another, and the place a subscription follows, so that it can be checked
// for a newer version. Kept in `space_links`, this library's own; the place
// a subscription follows goes with it to one's other computers (SpaceCarry, #51).
//
// A transport is named by its own word ("github", "folder") and an address
// is whatever that transport goes by; nothing here reads either.
namespace SpaceLinks {

enum class Role {
    Sent,     // a space published here, and where it was sent
    Followed, // a space subscribed to, and where it is followed from
};

struct Link {
    QString space;
    QString transport;
    QString address;
    bool operator==(const Link &) const = default;
};

// Remembers the place, in place of the one this space had before by the same
// transport. A subscription follows one place: one followed by another
// transport is forgotten.
void remember(const Library &library, Role role, const Link &link);
[[nodiscard]] std::optional<Link> of(const Library &library, const QString &space, Role role, const QString &transport = {});
[[nodiscard]] QList<Link> all(const Library &library, Role role);
// Every place the space had, as when a subscription is dropped.
void forget(const Library &library, const QString &space);

} // namespace SpaceLinks
