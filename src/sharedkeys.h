#pragma once

#include <QString>
#include <QtTypes>
#include <expected>
#include <span>
#include <vector>

#include "sharedspace.h"
#include "spacekey.h"

class Library;

// The keys of the shared spaces this library is in, as their member uses
// them (#49): kept beside the person (SpaceKeys), learnt from the space's
// rotations, changed when a member goes, and picked for each batch sealed.
//
// Which key seals what: an operation of this computer's is sealed with the
// newest key it holds, except what it wrote up to a rotation of its own,
// which goes with the key before it (so that whoever lacks the new one can
// still read up to it), and a rotation itself, which goes with the key the
// space was founded with: every member holds that one (an invite hands all
// of them over), and a rotation gives away nothing to whoever else holds it,
// as its key is sealed to each member it hands it to. So a member who was
// away reads up to a rotation, takes the new key from it, and reads on.
namespace SharedKeys {

// Where this user keeps the keys of their spaces: beside their person.
[[nodiscard]] QString folder(const Library &library);

// Every key of `space` held here, the founding one first.
[[nodiscard]] std::expected<std::vector<SpaceKey>, QString> held(const Library &library, const QString &space);

// Keeps each key that a rotation the space holds handed to this library's
// person, or to one it was before a pairing (#50), and that isn't kept yet.
// How many it kept.
[[nodiscard]] std::expected<qsizetype, QString> learn(const Library &library, const QString &space);

// Changes the key of `space`: a new one, kept here, sealed to every member
// now and handed to them in a rotation written by this library's person
// (who must be a writer), with what this library holds of each device as its
// cut-off.
[[nodiscard]] std::expected<void, QString> rotate(const Library &library, const QString &space);

// Whether this library, a writer of the space, should change its key again:
// when the newest key was handed to someone who isn't a member now (two
// writers removed someone each at once, each handing their key to the one
// the other removed), or this writer doesn't hold it, or someone was
// removed while the key never changed.
[[nodiscard]] bool needsRotating(const Library &library, const SharedSpace::State &state, std::span<const SpaceKey> keys);

// Of `keys` (held()), the one this library's operation `op` of the space is
// sealed with.
[[nodiscard]] const SpaceKey &sealing(const SharedSpace::State &state, std::span<const SpaceKey> keys, const SpaceOperation &op);

} // namespace SharedKeys
