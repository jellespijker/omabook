#pragma once

#include <QJsonObject>
#include <QString>
#include <expected>

#include "library.h"

// Pairing a computer into its person's personal space (ADR 0001, *People and
// keys*), as two of one's own computers replicate their logs: both keep the
// personal space with the smaller id (Spaces::pair), and one person, the one
// of the two with the smaller id. The computer whose person that is not is
// given the person's key over the ssh the sync already runs on, and keeps it
// in place of its own; from then on both sign as one person. What either
// signed before stays signed by the key it was signed with, and verifies.
//
// Any two of one's computers that replicate settle on the same person this
// way, also when two that were each paired with others meet: the smallest id
// of them all wins, and no computer goes back to a person it gave up.
namespace Pairing {

// What a side says of itself in its manifest and in a replication request:
// that it signs ("signs": 1), its person's id, and the computer's name.
[[nodiscard]] QJsonObject introduction(const Library &library);

// The person's key for the other side (`theirs`, its introduction), when its
// person is to become this one's: this side's has the smaller id, and the
// other side signs. Empty otherwise.
[[nodiscard]] QJsonObject keyFor(const Library &library, const QJsonObject &theirs);

// Becomes the other side's person when that is the one both are to be: its
// id is the smaller, and `theirs` gives its key. Answers whether it did, or
// why it couldn't.
[[nodiscard]] std::expected<bool, QString> take(Library &library, const QJsonObject &theirs);

} // namespace Pairing
