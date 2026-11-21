#pragma once

#include <QHash>
#include <QString>
#include <span>

#include "changelog.h"
#include "spacereplay.h"

// The names the members of a shared space go by there (#124), as each
// chose: one's own name travels in the space as an operation of one's own
// (entity "person", one's person id, field "name"), signed and judged by the
// role rules like the rest, so it goes wherever the space goes. An Omabook
// from before takes it and passes it over.
//
// A name is someone else's words, so it is untrusted: of each member only
// the name they gave themselves counts, the latest by clock, and only one
// that is a name (UntrustedText::name: plain, one line, at most
// UntrustedText::maxNameLength characters). A name one member gives
// another, or one longer than that, is passed over, never shortened; an
// empty one takes the name away.
namespace MemberNames {

inline constexpr auto entity = "person";
inline constexpr auto field = "name";

// Of a shared space's operations, the name each member last gave themselves: {person: name}.
[[nodiscard]] QHash<QString, QString> of(std::span<const SpaceOperation> ops);

// The operation by which `person` goes by `name` in the shared space `space`.
[[nodiscard]] ChangeLog::Entry entry(const QString &space, const QString &person, const QString &name);

} // namespace MemberNames
