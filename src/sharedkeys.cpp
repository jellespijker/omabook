#include "sharedkeys.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>

#include "keyrotation.h"
#include "library.h"
#include "person.h"
#include "spacekeys.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool holds(std::span<const SpaceKey> keys, const QString &id) {
    return std::ranges::any_of(keys, [&id](const SpaceKey &key) { return key.id() == id; });
}

// The key `rotation` handed to this library's person, or to one it was before a pairing.
[[nodiscard]] std::optional<SpaceKey> handedHere(const Library &library, const KeyRotation &rotation) {
    if (const Person *me = library.person(); me != nullptr && rotation.handedTo(me->id()))
        if (auto key = KeyRotations::open(rotation, *me))
            return std::move(*key);
    for (const QString &was : library.personFile().formerIds())
        if (rotation.handedTo(was))
            if (const auto former = library.personFile().former(was))
                if (auto key = KeyRotations::open(rotation, *former))
                    return std::move(*key);
    return std::nullopt;
}

} // namespace

QString SharedKeys::folder(const Library &library) { return QFileInfo(library.personFile().file()).absoluteDir().filePath(u"spaces"_s); }

std::expected<std::vector<SpaceKey>, QString> SharedKeys::held(const Library &library, const QString &space) {
    auto keys = SpaceKeys::all(folder(library), space);
    if (!keys)
        return std::unexpected(u"This computer doesn't hold the key of %1: %2"_s.arg(space, keys.error()));
    return keys;
}

std::expected<qsizetype, QString> SharedKeys::learn(const Library &library, const QString &space) {
    const auto keys = held(library, space);
    if (!keys)
        return std::unexpected(keys.error());
    qsizetype learnt = 0;
    for (const KeyRotation &rotation : SharedSpace::state(library, space).rotations) {
        if (holds(*keys, rotation.key))
            continue;
        if (const auto key = handedHere(library, rotation)) {
            const auto kept = SpaceKeys::keep(folder(library), space, *key);
            if (!kept)
                return std::unexpected(kept.error());
            learnt += *kept ? 1 : 0;
        }
    }
    return learnt;
}

std::expected<void, QString> SharedKeys::rotate(const Library &library, const QString &space) {
    const SharedSpace::State now = SharedSpace::state(library, space);
    QStringList members;
    for (const SharedSpace::Member &member : now.members)
        members << member.person;
    const KeyRotation *newest = now.newestRotation();
    auto key = SpaceKey::generate();
    if (!key)
        return std::unexpected(key.error());
    const auto entry =
        KeyRotations::entry(space, *key, newest != nullptr ? newest->generation + 1 : 1, SharedSpace::lastHeld(library, space), members);
    if (!entry)
        return std::unexpected(entry.error());
    // Kept first: a rotation written whose key this computer lost would leave
    // it reading the space only through its own sealed copy.
    if (const auto kept = SpaceKeys::keep(folder(library), space, *key); !kept)
        return std::unexpected(kept.error());
    return SharedSpace::write(library, space, std::span(&*entry, 1));
}

bool SharedKeys::needsRotating(const Library &library, const SharedSpace::State &state, std::span<const SpaceKey> keys) {
    const Person *me = library.person();
    if (me == nullptr || state.roleOf(me->id()) != SharedSpace::Role::Writer)
        return false;
    const KeyRotation *newest = state.newestRotation();
    // Someone removed while the key never changed: a removal from before
    // keys changed with it, or one whose change of key didn't get written.
    if (newest == nullptr)
        return std::ranges::any_of(state.accepted, [](const SpaceOperation &op) {
            return op.entity == "member"_L1 && op.field == "role"_L1 && SharedSpace::roleFrom(op.value) == SharedSpace::Role::None;
        });
    if (!holds(keys, newest->key))
        return true;
    for (auto person = newest->sealed.keyBegin(); person != newest->sealed.keyEnd(); ++person)
        if (state.roleOf(*person) == SharedSpace::Role::None)
            return true;
    return false;
}

const SpaceKey &SharedKeys::sealing(const SharedSpace::State &state, std::span<const SpaceKey> keys, const SpaceOperation &op) {
    Q_ASSERT(!keys.empty());
    const SpaceKey &founding = keys.front();
    if (KeyRotation::from(op))
        return founding;
    // The newest rotation held whose key this operation may be sealed with:
    // not one of this device's own at or after it.
    for (const KeyRotation &rotation : state.rotations | std::views::reverse) {
        if (rotation.op.device == op.device && rotation.op.seq >= op.seq)
            continue;
        if (const auto key = std::ranges::find_if(keys, [&rotation](const SpaceKey &one) { return one.id() == rotation.key; });
            key != keys.end())
            return *key;
    }
    return founding;
}
