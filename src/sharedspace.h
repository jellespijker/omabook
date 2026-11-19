#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QtTypes>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include "changelog.h"
#include "spacereplay.h"

class Library;

// A shared space (ADR 0001, *Spaces* and *People and keys*; #38): a space
// several people write, as a household library or a book club.
//
// Who may write it travels in it, as operations like the rest, ordered by
// their clocks: its founding (entity "space", field "founded": who founded
// it, a nonce, its name and parts), which makes the founder its first
// writer, and each change of a member's role (entity "member", the person's
// id, field "role": "writer", "reader" or "removed"). The space's id is a
// hash of the founder's id and the nonce, so its founding can't be forged
// by anyone else.
//
// The role rules: an operation is taken only when it is signed, by its
// author, and the author was a writer of the space at the operation's time:
// by the founding and the role changes before it, in the order of their
// clocks (then device, then number). Anything else is refused and reported,
// never kept or applied. A reader gets everything and writes nothing. A
// member removed has every later operation refused; what they wrote before
// stays. The rules are applied again to everything held each time the space
// is read, so a removal that arrives late still refuses what came after it.
//
// Like a published space, a shared space is a layer beside the library: its
// operations are kept under its id ("shared:…"), which keeps them out of
// what the library's rows are made of (`live_ops`), and it never travels to
// one's other computers by the ssh sync; it travels by its own transports,
// sealed with its key (SpaceFolder).
namespace SharedSpace {

inline constexpr auto kind = "shared";

enum class Role : std::uint8_t { None, Reader, Writer };
// "reader", "writer", or "" for none.
[[nodiscard]] QString roleName(Role role);
// Of "reader", "writer" or "removed" (none).
[[nodiscard]] std::optional<Role> roleFrom(QStringView name);

struct Member {
    QString person; // their id (Person::id)
    Role role = Role::None;
    qint64 since = 0; // the clock of the operation that gave them the role
};

struct Refused {
    SpaceOperation op;
    QString why;
};

// What the role rules make of a space's operations.
struct State {
    QString founder;
    QString name;
    QStringList parts; // the optional parts it holds (PublishedSpace::Parts), as founded
    QList<SpaceOperation> accepted;
    QList<Refused> refused;
    QList<Member> members; // as of the last operation, removed ones left out, in the order they joined

    [[nodiscard]] Role roleOf(const QString &person) const;
};

// The id of the space `founder` founds with `nonce`.
[[nodiscard]] QString idFor(const QString &founder, const QString &nonce);

// The role rules, applied to `ops`, of the space `space`, in any order.
[[nodiscard]] State admit(QStringView space, std::span<const SpaceOperation> ops);

// The shared spaces this library is a member of (recorded: founded or joined).
[[nodiscard]] QStringList spaces(const Library &library);
[[nodiscard]] bool isShared(const Library &library, const QString &space);
// Every operation held of the space, signatures and all.
[[nodiscard]] QList<SpaceOperation> held(const Library &library, const QString &space);
// Of each device of the space, the last number held.
[[nodiscard]] QHash<QString, qint64> lastHeld(const Library &library, const QString &space);
// What the role rules make of what is held.
[[nodiscard]] State state(const Library &library, const QString &space);

// Founds a space called `name` holding `parts`, its founder the library's
// person, and records it here. Its id.
[[nodiscard]] std::expected<QString, QString> found(const Library &library, const QString &name, const QStringList &parts);
// Records a space founded or joined (by an invite), so its operations are
// taken here; who founded it is known for sure only from its founding.
void record(const Library &library, const QString &space, const QString &name, const QString &founder = {});

// Appends `entries` to the space as the library's person, each signed and
// stamped now: refused unless they are a writer of it now.
[[nodiscard]] std::expected<void, QString> write(const Library &library, const QString &space, std::span<const ChangeLog::Entry> entries);
// Gives `person` the role `role` in the space (Role::None removes them).
[[nodiscard]] std::expected<void, QString> setRole(const Library &library, const QString &space, const QString &person, Role role);

// What taking operations from elsewhere did.
struct Taken {
    qsizetype added = 0; // kept here now
    qsizetype known = 0; // held already
    QList<Refused> refused;
};
// Takes `incoming` of the space in: of them, those the role rules accept
// with what is held, kept as their writers wrote them.
[[nodiscard]] Taken take(const Library &library, const QString &space, std::span<const SpaceOperation> incoming);

} // namespace SharedSpace
