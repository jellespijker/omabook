#include "spacecarry.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <sodium.h>
#include <utility>

#include "crypto.h"
#include "keyrotation.h"
#include "library.h"
#include "log.h"
#include "person.h"
#include "publishedspace.h"
#include "replication.h"
#include "sharedcollection.h"
#include "sharedkeys.h"
#include "sharedspace.h"
#include "sharedspaces.h"
#include "spacekey.h"
#include "spacekeys.h"
#include "spacelinks.h"
#include "spacesnapshot.h"
#include "subscribedspace.h"
#include "subscriptions.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto base64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

void wipe(QByteArray &bytes) { sodium_memzero(bytes.data(), static_cast<std::size_t>(bytes.size())); }

// What a subscription held comes to, to tell whether the other side holds
// the same of it: how many operations, and the latest clock among them.
[[nodiscard]] QString digestOf(const QList<SpaceOperation> &ops) {
    qint64 latest = 0;
    for (const SpaceOperation &op : ops)
        latest = std::max(latest, op.hlc);
    return u"%1:%2"_s.arg(ops.size()).arg(latest);
}

// Of each shared space, each device's last number held; of each subscription, its digest.
[[nodiscard]] QJsonObject heldHere(const Library &library) {
    QJsonObject shared;
    for (const QString &space : SharedSpace::spaces(library)) {
        QJsonObject devices;
        const QHash<QString, qint64> last = SharedSpace::lastHeld(library, space);
        for (auto device = last.cbegin(); device != last.cend(); ++device)
            devices.insert(device.key(), device.value());
        shared.insert(space, devices);
    }
    QJsonObject subscribed;
    for (const SubscribedSpace::Subscription &subscription : SubscribedSpace::list(library))
        subscribed.insert(subscription.space, digestOf(SubscribedSpace::operations(library, subscription.space)));
    return {{"shared", shared}, {"subscribed", subscribed}};
}

// `object`, sealed so that only `person` opens it; the plaintext wiped once sealed.
[[nodiscard]] std::optional<QByteArray> sealed(const Person &person, const QJsonObject &object) {
    QByteArray plain = QJsonDocument(object).toJson(QJsonDocument::Compact);
    auto box = Person::sealFor(person.publicKey(), plain);
    wipe(plain);
    if (!box) {
        qCWarning(lcSync).noquote() << "the spaces to carry could not be sealed:" << box.error();
        return std::nullopt;
    }
    return std::move(*box);
}

[[nodiscard]] QJsonObject opened(const Person &person, const QByteArray &box) {
    auto plain = person.openSealed(box);
    if (!plain)
        return {};
    const QJsonObject object = QJsonDocument::fromJson(*plain).object();
    wipe(*plain);
    return object;
}

// Whether `box` is as `person` signed it: anyone may seal to a person's key, only they sign with it.
[[nodiscard]] bool signedBy(const Person &person, const QByteArray &box, const QByteArray &signature) {
    Person::Signature bytes{};
    if (std::cmp_not_equal(signature.size(), bytes.size()))
        return false;
    std::ranges::copy(Crypto::bytes(signature), bytes.begin());
    return Person::verify(person.publicKey(), Crypto::bytes(box), bytes);
}

[[nodiscard]] QByteArray decoded(const QJsonValue &value) { return QByteArray::fromBase64(value.toString().toLatin1(), base64); }

[[nodiscard]] QString linkedPath(const Library &library, const QString &space) {
    const auto collection = library.collection(SubscribedSpace::linkedCollection(library, space));
    return collection ? collection->path : QString();
}

// Every generation of the space's key held here, oldest first, each an
// object of its own: the key, its id and its generation (0 for the one the
// space was founded with; that of the rotation that handed it, #49).
[[nodiscard]] QJsonArray keysOf(const Library &library, const QString &space) {
    const auto keys = SharedKeys::held(library, space);
    if (!keys)
        return {};
    QHash<QString, qint64> generations;
    for (const KeyRotation &rotation : SharedSpace::state(library, space).rotations)
        generations.insert(rotation.key, rotation.generation);
    QList<std::pair<qint64, QJsonObject>> entries;
    for (const SpaceKey &key : *keys) {
        QByteArray raw = Crypto::toByteArray(key.bytes());
        QByteArray encoded = raw.toBase64(base64);
        const qint64 generation = &key == &keys->front() ? 0 : generations.value(key.id(), 1);
        entries.append({generation, QJsonObject{{"key", QString::fromLatin1(encoded)}, {"id", key.id()}, {"generation", generation}}});
        wipe(raw);
        wipe(encoded);
    }
    std::ranges::stable_sort(entries, {}, &std::pair<qint64, QJsonObject>::first);
    QJsonArray array;
    for (const auto &entry : std::as_const(entries))
        array << entry.second;
    return array;
}

// Keeps each of `keys` not here yet, oldest generation first, so that the
// one the space was founded with is kept as that (SpaceKeys::keep), never
// over one that is here. How many it kept.
[[nodiscard]] qsizetype keepKeys(const Library &library, const QString &space, const QJsonArray &keys, QStringList &problems) {
    QList<std::pair<qint64, QJsonObject>> entries;
    for (const auto &entry : keys)
        entries.append({entry.toObject().value("generation").toInteger(), entry.toObject()});
    std::ranges::stable_sort(entries, {}, &std::pair<qint64, QJsonObject>::first);
    qsizetype keptHere = 0;
    for (const auto &entry : std::as_const(entries)) {
        QByteArray raw = decoded(entry.second.value("key"));
        const auto key = SpaceKey::fromBytes(Crypto::bytes(raw));
        wipe(raw);
        if (!key)
            continue;
        const auto kept = SpaceKeys::keep(SharedKeys::folder(library), space, *key);
        if (!kept) {
            problems << u"The key of %1 could not be kept here: %2"_s.arg(space, kept.error());
            return keptHere;
        }
        keptHere += *kept ? 1 : 0;
    }
    if (!SpaceKey::load(SharedSpaces::keyFile(library, space)))
        problems << u"The key of %1 was not carried here, so its folder can't be read here yet."_s.arg(space);
    return keptHere;
}

[[nodiscard]] QJsonArray sharedFor(Library &library, const QJsonObject &held) {
    QJsonArray shared;
    for (const QString &space : SharedSpace::spaces(library)) {
        const QJsonObject last = held.value(space).toObject();
        QList<SpaceOperation> lacking = SharedSpace::held(library, space);
        lacking.removeIf([&last](const SpaceOperation &op) { return op.seq <= last.value(op.device).toInteger(); });
        // In it there already, with its key, and lacking nothing.
        if (held.contains(space) && lacking.isEmpty())
            continue;
        const SharedSpace::State state = SharedSpace::state(library, space);
        shared << QJsonObject{{"space", space},
                              {"name", state.name},
                              {"collection", linkedPath(library, space)},
                              {"keys", keysOf(library, space)},
                              {"ops", Replication::toJson(lacking)}};
    }
    return shared;
}

[[nodiscard]] QJsonArray subscribedFor(const Library &library, const QJsonObject &held) {
    QJsonArray subscribed;
    for (const SubscribedSpace::Subscription &subscription : SubscribedSpace::list(library)) {
        const QList<SpaceOperation> ops = SubscribedSpace::operations(library, subscription.space);
        if (held.value(subscription.space).toString() == digestOf(ops))
            continue;
        QJsonObject entry{{"space", subscription.space},
                          {"name", subscription.name},
                          {"owner", subscription.owner},
                          {"owner_name", subscription.ownerName},
                          {"collection", linkedPath(library, subscription.space)},
                          {"ops", Replication::toJson(ops)}};
        if (const auto link = SpaceLinks::of(library, subscription.space, SpaceLinks::Role::Followed))
            entry.insert("followed", QJsonObject{{"transport", link->transport}, {"address", link->address}});
        subscribed << entry;
    }
    return subscribed;
}

void takeShared(Library &library, const QJsonObject &entry, SpaceCarry::Taken &taken) {
    const QString space = entry.value("space").toString();
    if (!space.startsWith("shared:"_L1))
        return;
    taken.keys += keepKeys(library, space, entry.value("keys").toArray(), taken.problems);
    const Library::Batch batch(library);
    if (!SharedSpace::isShared(library, space)) {
        SharedSpace::record(library, space, entry.value("name").toString());
        ++taken.spaces;
    }
    const QList<SpaceOperation> before = SharedSpace::state(library, space).accepted;
    const SharedSpace::Taken took = SharedSpace::take(library, space, Replication::anyOperationsFrom(entry.value("ops").toArray()));
    taken.operations += took.added;
    taken.refused += took.refused.size();
    for (const SharedSpace::Refused &refused : took.refused)
        taken.problems << u"An operation of %1 was refused: %2"_s.arg(space, refused.why);
    // A membership of a person given up at a pairing, handed to the one now, once it is here (#50).
    if (const auto carried = SharedSpace::carryOver(library, space); !carried)
        taken.problems << carried.error();
    const QString as = SubscribedSpace::linkedCollection(library, space) > 0 ? QString() : entry.value("collection").toString();
    if (const auto filed = SharedCollection::file(library, space, before, as); !filed)
        taken.problems << u"The books of %1 could not be filed here: %2"_s.arg(space, filed.error());
}

[[nodiscard]] SpaceSnapshot snapshotOf(const QJsonObject &entry) {
    SpaceSnapshot snapshot{.space = entry.value("space").toString(),
                           .kind = QString::fromLatin1(PublishedSpace::kind),
                           .name = entry.value("name").toString(),
                           .owner = {.name = entry.value("owner_name").toString(), .person = entry.value("owner").toString()},
                           .parts = {},
                           .clock = 0,
                           .writtenBy = {},
                           .operations = Replication::anyOperationsFrom(entry.value("ops").toArray())};
    for (const SpaceOperation &op : std::as_const(snapshot.operations))
        snapshot.clock = std::max(snapshot.clock, op.hlc);
    return snapshot;
}

void takeSubscribed(Library &library, const QJsonObject &entry, SpaceCarry::Taken &taken) {
    const SpaceSnapshot snapshot = snapshotOf(entry);
    // Given up since on one of one's computers: what it says of that wins (SubscribedSpace::followed).
    if (!SubscribedSpace::followed(library, snapshot.space).value_or(true))
        return;
    const bool before = SubscribedSpace::isSubscribed(library, snapshot.space);
    const QString as = SubscribedSpace::linkedCollection(library, snapshot.space) > 0 ? QString() : entry.value("collection").toString();
    const Library::Batch batch(library);
    if (const auto subscribed = Subscriptions::subscribe(library, snapshot, {.as = as, .dryRun = false}); !subscribed) {
        taken.problems << u"The subscription to %1 could not be taken here: %2"_s.arg(snapshot.name, subscribed.error());
        return;
    }
    taken.subscriptions += before ? 0 : 1;
    const QJsonObject followed = entry.value("followed").toObject();
    if (!followed.isEmpty() && !SpaceLinks::of(library, snapshot.space, SpaceLinks::Role::Followed))
        SpaceLinks::remember(library, SpaceLinks::Role::Followed,
                             {.space = snapshot.space,
                              .transport = followed.value("transport").toString(),
                              .address = followed.value("address").toString()});
}

// Drops each subscription the person said, last, on any of their computers, they no longer follow.
[[nodiscard]] qsizetype settle(const Library &library) {
    qsizetype dropped = 0;
    for (const SubscribedSpace::Subscription &subscription : SubscribedSpace::list(library))
        if (!SubscribedSpace::followed(library, subscription.space).value_or(true) && SubscribedSpace::drop(library, subscription.space))
            ++dropped;
    return dropped;
}

} // namespace

QJsonObject SpaceCarry::introduction(const Library &library) {
    const Person *me = library.person();
    if (me == nullptr)
        return {};
    QJsonObject introduction{{"carries", 1}, {"carrier", me->id()}};
    if (const auto box = sealed(*me, heldHere(library)))
        introduction.insert("carry", QString::fromLatin1(box->toBase64(base64)));
    return introduction;
}

bool SpaceCarry::isOneself(const Library &library, const QJsonObject &theirs) {
    const Person *me = library.person();
    return me != nullptr && theirs.value("carries").toInt() >= 1 && theirs.value("carrier").toString() == me->id();
}

bool SpaceCarry::worthCarrying(const Library &library, const QJsonObject &theirs) {
    if (!isOneself(library, theirs))
        return false;
    if (!SharedSpace::spaces(library).isEmpty() || !SubscribedSpace::list(library).isEmpty())
        return true;
    const QJsonObject held = opened(*library.person(), decoded(theirs.value("carry")));
    return !held.value("shared").toObject().isEmpty() || !held.value("subscribed").toObject().isEmpty();
}

QJsonObject SpaceCarry::offer(Library &library, const QJsonObject &theirs) {
    if (!isOneself(library, theirs))
        return {};
    const Person *me = library.person();
    // What they hold, as they said it to the person; all of it when that can't be read.
    const QJsonObject held = opened(*me, decoded(theirs.value("carry")));
    const QJsonArray shared = sharedFor(library, held.value("shared").toObject());
    const QJsonArray subscribed = subscribedFor(library, held.value("subscribed").toObject());
    if (shared.isEmpty() && subscribed.isEmpty())
        return {};
    const auto box = sealed(*me, {{"shared", shared}, {"subscribed", subscribed}});
    if (!box)
        return {};
    const Person::Signature signature = me->sign(Crypto::bytes(*box));
    return {{"carried", QString::fromLatin1(box->toBase64(base64))},
            {"carried_signature", QString::fromLatin1(Crypto::toByteArray(signature).toBase64(base64))}};
}

SpaceCarry::Taken SpaceCarry::take(Library &library, const QJsonObject &theirs) {
    Taken taken;
    taken.dropped = settle(library);
    const Person *me = library.person();
    if (me == nullptr || !theirs.contains("carried"))
        return taken;
    const QByteArray box = decoded(theirs.value("carried"));
    if (!signedBy(*me, box, decoded(theirs.value("carried_signature")))) {
        taken.problems << u"What the other computer carried of the spaces you are in is not signed by you, so none of it was taken."_s;
        return taken;
    }
    const QJsonObject carried = opened(*me, box);
    for (const auto &entry : carried.value("shared").toArray())
        takeShared(library, entry.toObject(), taken);
    for (const auto &entry : carried.value("subscribed").toArray())
        takeSubscribed(library, entry.toObject(), taken);
    return taken;
}
