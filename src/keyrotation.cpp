#include "keyrotation.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

constexpr auto base64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

[[nodiscard]] bool isKeyId(QStringView id) {
    static const QRegularExpression pattern(u"^[0-9a-f]{32}$"_s);
    return pattern.matchView(id).hasMatch();
}

// Two rotations made at once (neither writer held the other) where either
// leaves the other's writer out.
[[nodiscard]] bool conflict(const KeyRotation &one, const KeyRotation &other) {
    return one.isPast(other.op) && other.isPast(one.op) && (!one.handedTo(other.op.author) || !other.handedTo(one.op.author));
}

} // namespace

std::optional<KeyRotation> KeyRotation::from(const SpaceOperation &op) {
    if (op.entity != QLatin1StringView(KeyRotations::entity) || op.field != QLatin1StringView(KeyRotations::field) || !isKeyId(op.id))
        return std::nullopt;
    const QJsonObject value = QJsonDocument::fromJson(op.value.toUtf8()).object();
    KeyRotation rotation{.op = op, .generation = value.value("generation").toInteger(), .key = op.id, .cutOff = {}, .sealed = {}};
    const QJsonObject cutOff = value.value("cutoff").toObject();
    for (auto device = cutOff.constBegin(); device != cutOff.constEnd(); ++device)
        rotation.cutOff.insert(device.key(), device.value().toInteger());
    const QJsonObject sealed = value.value("sealed").toObject();
    for (auto person = sealed.constBegin(); person != sealed.constEnd(); ++person)
        rotation.sealed.insert(person.key(), QByteArray::fromBase64(person.value().toString().toLatin1(), QByteArray::Base64UrlEncoding));
    if (rotation.generation < 1 || rotation.sealed.isEmpty())
        return std::nullopt;
    return rotation;
}

qint64 KeyRotation::lastBefore(const QString &device) const { return device == op.device ? op.seq : cutOff.value(device, 0); }

std::expected<ChangeLog::Entry, QString> KeyRotations::entry(const QString &space, const SpaceKey &key, qint64 generation,
                                                             const QHash<QString, qint64> &cutOff, const QStringList &members) {
    const Crypto::WipedBytes keyBytes(Crypto::toByteArray(key.bytes()));
    QJsonObject sealed;
    for (const QString &member : members) {
        const auto to = Person::publicKeyFrom(member);
        if (!to)
            return std::unexpected(to.error());
        const auto box = Person::sealFor(*to, keyBytes.bytes());
        if (!box)
            return std::unexpected(box.error());
        sealed.insert(member, QString::fromLatin1(box->toBase64(base64)));
    }
    if (sealed.isEmpty())
        return std::unexpected(u"A space's key is changed for its members, and it has none."_s);
    QJsonObject devices;
    for (auto device = cutOff.constBegin(); device != cutOff.constEnd(); ++device)
        devices.insert(device.key(), device.value());
    const QJsonObject value{{"generation", generation}, {"cutoff", devices}, {"sealed", sealed}};
    return ChangeLog::Entry{.space = space,
                            .entity = QString::fromLatin1(entity),
                            .id = key.id(),
                            .field = QString::fromLatin1(field),
                            .value = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)),
                            .hlc = std::nullopt};
}

std::expected<SpaceKey, QString> KeyRotations::open(const KeyRotation &rotation, const Person &me) {
    const auto box = rotation.sealed.constFind(me.id());
    if (box == rotation.sealed.cend())
        return std::unexpected(u"The space's key was changed without handing the new one to you."_s);
    auto opened = me.openSealed(*box);
    if (!opened)
        return std::unexpected(opened.error());
    const Crypto::WipedBytes bytes(std::move(*opened));
    auto key = SpaceKey::fromBytes(Crypto::bytes(bytes.bytes()));
    if (key && key->id() != rotation.key)
        return std::unexpected(u"The key handed to you is not the one the change of key names."_s);
    return key;
}

QList<KeyRotation> KeyRotations::prevailing(QList<std::pair<KeyRotation, qint64>> standing) {
    std::ranges::sort(standing, [](const auto &one, const auto &other) {
        return std::tie(one.second, one.first.op.hlc, one.first.op.device, one.first.op.seq)
               < std::tie(other.second, other.first.op.hlc, other.first.op.device, other.first.op.seq);
    });
    QList<KeyRotation> prevail;
    for (const auto &[rotation, since] : std::as_const(standing))
        if (std::ranges::none_of(prevail, [&rotation](const KeyRotation &before) { return conflict(before, rotation); }))
            prevail << rotation;
    return prevail;
}

KeyRotations::CutOffs::CutOffs(const QList<KeyRotation> &rotations) {
    for (const KeyRotation &rotation : rotations) {
        After after{.rotation = rotation, .writers = {}};
        for (auto person = rotation.sealed.keyBegin(); person != rotation.sealed.keyEnd(); ++person)
            after.writers.insert(*person);
        m_after << after;
    }
}

std::optional<QString> KeyRotations::CutOffs::refusal(const SpaceOperation &op) const {
    for (const After &after : m_after)
        if (after.rotation.isPast(op) && !after.writers.contains(op.author))
            return u"It comes after the space's key was changed (generation %1), from someone the new key wasn't handed to: it "
                   "was sealed with an older key, after that key's time."_s.arg(after.rotation.generation);
    return std::nullopt;
}

void KeyRotations::CutOffs::madeMember(const SpaceOperation &op, const QString &person) {
    for (After &after : m_after)
        if (after.rotation.isPast(op))
            after.writers.insert(person);
}
