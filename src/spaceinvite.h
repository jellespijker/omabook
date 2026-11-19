#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <expected>
#include <span>
#include <vector>

#include "person.h"
#include "spacekey.h"

// An invitation to a shared space (ADR 0001, *People and keys*; #38): what
// hands a new member the space's key.
struct SpaceInvite {
    QString space; // its id, "shared:…"
    QString name;  // what it is called
    QString role;  // "reader" or "writer", as the inviter made them a member
    QString from;  // the inviter's person id
    QString to;    // the invitee's
};

// The invite file, `.omabook-invite`: JSON that says only its format and whom
// it is for ("to"), and a sealed box (Person::sealFor) to the invitee's key
// that only they can open. Inside: the invite and the space's keys, signed
// by the inviter (over "omabook-invite", a version byte, the keys and the
// invite), so the invitee knows whom it is from and that it was meant for
// them, and an invite sealed again for someone else is refused.
namespace SpaceInvites {

inline constexpr auto suffix = ".omabook-invite";

// The invite file for `invite`, handing over `keys`: every key of the space
// the inviter holds, the founding one first (SpaceKeys::all), so that the
// invitee reads what was sealed before each change of the key too (#49).
[[nodiscard]] std::expected<QByteArray, QString> write(const Person &from, const SpaceInvite &invite, std::span<const SpaceKey> keys);

struct Opened {
    SpaceInvite invite;
    std::vector<SpaceKey> keys; // the founding one first
};
// The invite in `file`, opened by `me`: refused when it is for someone else,
// was changed, or isn't signed by whom it says it is from.
[[nodiscard]] std::expected<Opened, QString> open(const Person &me, QByteArrayView file);

} // namespace SpaceInvites
