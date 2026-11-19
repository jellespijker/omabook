#pragma once

#include <QString>
#include <QStringView>
#include <QtTypes>
#include <expected>
#include <span>
#include <vector>

#include "spacekey.h"

// Every key of a shared space that its member holds (#49): the one it was
// founded with, and each one a key rotation (KeyRotation) handed them since.
// The old ones are kept, never replaced, so the batches sealed with them stay
// readable; which of them is the newest is for the space to say (its
// rotations), not the files.
//
// They are kept in the user's Omabook data folder, each in a file of its own
// for its owner alone (SpaceKey::keepIn, SecretFile): the founding one where
// it always was, `<id>.key` (SpaceKey::fileFor), and each one after it as
// `<id>.<its key id>.key`. Whatever carries a member's spaces to their other
// computers carries all of them (all(), keep()).
namespace SpaceKeys {

// Every key of `space` kept in `folder`, the founding one first. Refused when
// none is, or one of them can't be read as its owner's alone.
[[nodiscard]] std::expected<std::vector<SpaceKey>, QString> all(const QString &folder, QStringView space);

// Keeps `key` among those of `space` in `folder`: as the founding one when
// none is kept yet, else beside it. Whether it was not kept there already.
[[nodiscard]] std::expected<bool, QString> keep(const QString &folder, QStringView space, const SpaceKey &key);

// Keeps each of `keys`, as all() gives them (the founding one first). How
// many were not kept there already.
[[nodiscard]] std::expected<qsizetype, QString> keepAll(const QString &folder, QStringView space, std::span<const SpaceKey> keys);

} // namespace SpaceKeys
