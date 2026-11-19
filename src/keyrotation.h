#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QtTypes>
#include <expected>
#include <optional>
#include <utility>

#include "changelog.h"
#include "person.h"
#include "spacekey.h"
#include "spacereplay.h"

// A change of a shared space's key (#49; ADR 0001, *People and keys*): what
// a writer writes on removing a member, so that what comes after is sealed
// with a key the one removed doesn't hold, and so that they can't slip in
// an operation dated before their removal either.
//
// It is an operation of the space (entity "key", the new key's id, field
// "rotated"), signed like any other, whose value says:
// - its generation, one more than the newest its writer knew of;
// - its cut-off: of each device, the last number its writer held (a version
//   vector), and of its own device, itself: what was written with the keys
//   before it;
// - the new key, sealed (Person::sealFor) to each member it was handed to.
//
// The cut-off rule: an operation past a rotation's cut-off on its device (or
// of a device it doesn't name) is taken only from someone the new key was
// handed to, or someone made a member after it by one who was. Past the
// cut-off nobody else can have sealed it with the new key, so it can only
// have come under an old one: a removed writer's operation is refused there
// whatever its clock says. (The space's founding is never cut off.)
struct KeyRotation {
    SpaceOperation op;
    qint64 generation = 0;
    QString key;                       // the new key's id (SpaceKey::id)
    QHash<QString, qint64> cutOff;     // device → its last number before the rotation
    QHash<QString, QByteArray> sealed; // person id → the new key, sealed to them

    // The rotation `op` is, or none for an operation that is not one, or is
    // not one of a kind this Omabook knows.
    [[nodiscard]] static std::optional<KeyRotation> from(const SpaceOperation &op);

    // The last number of `device` before the rotation.
    [[nodiscard]] qint64 lastBefore(const QString &device) const;
    // Whether `other` comes after the rotation on its device.
    [[nodiscard]] bool isPast(const SpaceOperation &other) const { return other.seq > lastBefore(other.device); }
    [[nodiscard]] bool handedTo(const QString &person) const { return sealed.contains(person); }
    // The newer of two rotations: the higher generation, then the higher key
    // id, so that every member picks the same of two made at once.
    [[nodiscard]] bool isNewerThan(const KeyRotation &other) const {
        return std::pair(generation, key) > std::pair(other.generation, other.key);
    }
};

namespace KeyRotations {

inline constexpr auto entity = "key";
inline constexpr auto field = "rotated";

// The entry that hands `key`, of generation `generation`, to each of
// `members` (person ids), with the cut-off `cutOff`, for the space `space`.
[[nodiscard]] std::expected<ChangeLog::Entry, QString> entry(const QString &space, const SpaceKey &key, qint64 generation,
                                                             const QHash<QString, qint64> &cutOff, const QStringList &members);

// The key `rotation` handed to `me`, refused when it handed none to them or
// what they open is not the key it names.
[[nodiscard]] std::expected<SpaceKey, QString> open(const KeyRotation &rotation, const Person &me);

// Of rotations that each stand (their writer was a writer by what they held
// then), with how long their writer had been one (the clock of the role
// they held), those that prevail: all, unless two made at once each leave
// the other's writer out (each removed the other). Then the one whose writer
// has been a writer longer prevails, so a removed writer who claims to have
// removed their remover at the same time doesn't win by it.
[[nodiscard]] QList<KeyRotation> prevailing(QList<std::pair<KeyRotation, qint64>> standing);

// The cut-off rule over the rotations that prevail, as the role rules read
// the space's operations in order.
class CutOffs {
public:
    explicit CutOffs(const QList<KeyRotation> &rotations);

    // Why `op` is refused by a rotation it comes after; nothing when none refuses it.
    [[nodiscard]] std::optional<QString> refusal(const SpaceOperation &op) const;
    // That `op`, taken, made `person` a member: past a rotation's cut-off
    // they write after it too (whoever made them one could hand them its key).
    void madeMember(const SpaceOperation &op, const QString &person);

private:
    struct After {
        KeyRotation rotation;
        QSet<QString> writers; // who may write past its cut-off
    };
    QList<After> m_after;
};

} // namespace KeyRotations
