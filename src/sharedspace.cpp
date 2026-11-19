#include "sharedspace.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <tuple>

#include "crypto.h"
#include "hlc.h"
#include "library.h"
#include "person.h"
#include "signedops.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

using SharedSpace::Member;
using SharedSpace::Refused;
using SharedSpace::Role;
using SharedSpace::State;

[[nodiscard]] bool isFoundingLike(const SpaceOperation &op) { return op.entity == "space"_L1 && op.field == "founded"_L1; }

[[nodiscard]] QJsonObject object(const QString &json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }

// Whether `op` is the founding of `space`: by the person whose id, with its
// nonce, the space's id is made of.
[[nodiscard]] bool founds(const SpaceOperation &op, QStringView space) {
    if (!isFoundingLike(op) || op.id != space)
        return false;
    const QJsonObject founding = object(op.value);
    const QString founder = founding.value("founder").toString();
    return !founder.isEmpty() && op.author == founder && SharedSpace::idFor(founder, founding.value("nonce").toString()) == space;
}

[[nodiscard]] QString sameness(const SpaceOperation &op) { return op.device + u'\x1f' + QString::number(op.seq); }

// The operations to judge, in the order the rules read them; those that
// can't be judged at all are refused on the way.
[[nodiscard]] QList<SpaceOperation> signedOnly(QStringView space, std::span<const SpaceOperation> ops, QList<Refused> &refused) {
    QList<SpaceOperation> judged;
    for (const SpaceOperation &op : ops) {
        if (op.space != space)
            refused << Refused{.op = op, .why = u"It is of another space."_s};
        else if (op.signature.isEmpty())
            refused << Refused{.op = op, .why = u"It is not signed: a shared space takes only what its writers signed."_s};
        else if (!SignedOps::verify(op))
            refused << Refused{.op = op, .why = u"It is not as its author signed it."_s};
        else
            judged << op;
    }
    std::ranges::sort(judged, [](const SpaceOperation &one, const SpaceOperation &other) {
        return std::tie(one.hlc, one.device, one.seq) < std::tie(other.hlc, other.device, other.seq);
    });
    return judged;
}

[[nodiscard]] QString whyNotWriter(Role role, const QString &author) {
    const QString who = author.left(12) + u"…"_s;
    return role == Role::Reader ? u"%1 was a reader of this space then, and a reader doesn't write."_s.arg(who)
                                : u"%1 was not a member of this space then."_s.arg(who);
}

// The role an operation gives a member, or none for one that gives none.
[[nodiscard]] std::optional<Role> roleGiven(const SpaceOperation &op) {
    if (op.entity != "member"_L1 || op.field != "role"_L1 || !Person::publicKeyFrom(op.id))
        return std::nullopt;
    return SharedSpace::roleFrom(op.value);
}

// Why an operation by an author of `role` is refused; nothing when it is taken.
[[nodiscard]] std::optional<QString> refusal(const SpaceOperation &op, Role role) {
    if (role != Role::Writer)
        return whyNotWriter(role, op.author);
    if (isFoundingLike(op))
        return u"A space is founded once."_s;
    if (op.entity == "member"_L1 && !roleGiven(op))
        return u"It gives a member no role this Omabook knows."_s;
    return std::nullopt;
}

// The founding found, the roles as the operations change them, in order.
void judge(const QList<SpaceOperation> &judged, qsizetype founding, State &state) {
    const QJsonObject founded = object(judged.at(founding).value);
    state.founder = judged.at(founding).author;
    state.name = founded.value("name").toString();
    for (const auto &part : founded.value("parts").toArray())
        state.parts << part.toString();
    QHash<QString, Member> roles{{state.founder, Member{.person = state.founder, .role = Role::Writer, .since = judged.at(founding).hlc}}};
    QList<QString> order{state.founder};
    for (qsizetype at = 0; at < judged.size(); ++at) {
        const SpaceOperation &op = judged.at(at);
        const std::optional<QString> why = at == founding ? std::nullopt : refusal(op, roles.value(op.author).role);
        if (why) {
            state.refused << Refused{.op = op, .why = *why};
            continue;
        }
        state.accepted << op;
        if (const std::optional<Role> given = at == founding ? std::nullopt : roleGiven(op)) {
            if (!roles.contains(op.id))
                order << op.id;
            roles.insert(op.id, Member{.person = op.id, .role = *given, .since = op.hlc});
        }
    }
    for (const QString &person : std::as_const(order))
        if (const Member member = roles.value(person); member.role != Role::None)
            state.members << member;
}

// Keeps an operation as its writer wrote it, under their device and number.
void keep(const Library &library, const SpaceOperation &op) {
    const qint64 space = ChangeLog::internId(library, op.space);
    const qint64 device = ChangeLog::internId(library, op.device);
    ChangeLog::exec(library,
                    "INSERT OR IGNORE INTO ops (space, device, seq, hlc, author, entity, id, field, value, signer, signature) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    {space, device, op.seq, Hlc::normalize(op.hlc), ChangeLog::internId(library, op.author), op.entity,
                     ChangeLog::internId(library, op.id), op.field, op.value, ChangeLog::internId(library, op.signer), op.signature});
    ChangeLog::exec(library,
                    "INSERT INTO seen (space, device, seq, hlc) VALUES (?, ?, ?, ?) ON CONFLICT(space, device) DO UPDATE SET "
                    "seq = MAX(seq, excluded.seq), hlc = MAX(hlc, excluded.hlc)",
                    {space, device, op.seq, Hlc::normalize(op.hlc)});
}

} // namespace

QString SharedSpace::roleName(Role role) {
    switch (role) {
    case Role::Reader:
        return u"reader"_s;
    case Role::Writer:
        return u"writer"_s;
    case Role::None:
        break;
    }
    return {};
}

std::optional<Role> SharedSpace::roleFrom(QStringView name) {
    if (name == "reader"_L1)
        return Role::Reader;
    if (name == "writer"_L1)
        return Role::Writer;
    if (name == "removed"_L1)
        return Role::None;
    return std::nullopt;
}

Role SharedSpace::State::roleOf(const QString &person) const {
    const auto member = std::ranges::find_if(members, [&person](const Member &one) { return one.person == person; });
    return member != members.cend() ? member->role : Role::None;
}

QString SharedSpace::idFor(const QString &founder, const QString &nonce) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData("omabook shared space\x1f");
    hash.addData(founder.toUtf8());
    hash.addData(QByteArrayView("\x1f", 1));
    hash.addData(nonce.toUtf8());
    return u"shared:"_s + QString::fromLatin1(hash.result().toHex().left(32));
}

State SharedSpace::admit(QStringView space, std::span<const SpaceOperation> ops) {
    State state;
    const QList<SpaceOperation> judged = signedOnly(space, ops, state.refused);
    const auto founding = std::ranges::find_if(judged, [space](const SpaceOperation &op) { return founds(op, space); });
    if (founding == judged.cend()) {
        for (const SpaceOperation &op : judged)
            state.refused << Refused{.op = op, .why = u"The space's founding isn't here yet, so nobody is a writer of it."_s};
        return state;
    }
    judge(judged, std::ranges::distance(judged.cbegin(), founding), state);
    return state;
}

QStringList SharedSpace::spaces(const Library &library) {
    QStringList ids;
    for (QSqlQuery query = ChangeLog::query(library, "SELECT id FROM spaces WHERE kind = ? ORDER BY name, id", {QString::fromLatin1(kind)});
         query.next();)
        ids << query.value(0).toString();
    return ids;
}

bool SharedSpace::isShared(const Library &library, const QString &space) {
    QSqlQuery query = ChangeLog::query(library, "SELECT 1 FROM spaces WHERE id = ? AND kind = ?", {space, QString::fromLatin1(kind)});
    return query.next();
}

QList<SpaceOperation> SharedSpace::held(const Library &library, const QString &space) {
    QList<SpaceOperation> ops;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT d.name, o.seq, o.hlc, a.name, o.entity, i.name, o.field, o.value, g.name, o.signature "
                                            "FROM space_ops AS o JOIN log_ids AS d ON d.id = o.device JOIN log_ids AS a ON a.id = o.author "
                                            "JOIN log_ids AS i ON i.id = o.id LEFT JOIN log_ids AS g ON g.id = o.signer "
                                            "WHERE o.space = (SELECT id FROM log_ids WHERE name = ?) ORDER BY d.name, o.seq",
                                            {space});
         query.next();)
        ops.append({.space = space,
                    .device = query.value(0).toString(),
                    .seq = query.value(1).toLongLong(),
                    .hlc = query.value(2).toLongLong(),
                    .author = query.value(3).toString(),
                    .entity = query.value(4).toString(),
                    .id = query.value(5).toString(),
                    .field = query.value(6).toString(),
                    .value = query.value(7).toString(),
                    .signer = query.value(8).toString(),
                    .signature = query.value(9).toByteArray()});
    return ops;
}

QHash<QString, qint64> SharedSpace::lastHeld(const Library &library, const QString &space) {
    QHash<QString, qint64> last;
    for (const SpaceOperation &op : held(library, space))
        last[op.device] = std::max(last.value(op.device), op.seq);
    return last;
}

State SharedSpace::state(const Library &library, const QString &space) {
    const QList<SpaceOperation> ops = held(library, space);
    return admit(space, ops);
}

std::expected<QString, QString> SharedSpace::found(const Library &library, const QString &name, const QStringList &parts) {
    const Person *founder = library.person();
    if (founder == nullptr)
        return std::unexpected(u"A shared space is founded by a person, and this library has none (omabook identity says why)."_s);
    if (name.simplified().isEmpty())
        return std::unexpected(u"A shared space needs a name."_s);
    if (!Crypto::ready())
        return std::unexpected(u"libsodium could not be made ready: the system gives it no randomness."_s);
    QByteArray nonce(16, Qt::Uninitialized);
    randombytes_buf(nonce.data(), static_cast<std::size_t>(nonce.size()));
    const QString nonceText = QString::fromLatin1(nonce.toHex());
    QString space = idFor(founder->id(), nonceText);
    const Library::Batch batch(library);
    record(library, space, name.simplified(), founder->id());
    ChangeLog::append(library, {.space = space,
                                .entity = u"space"_s,
                                .id = space,
                                .field = u"founded"_s,
                                .value = QString::fromUtf8(QJsonDocument(QJsonObject{{"founder", founder->id()},
                                                                                     {"nonce", nonceText},
                                                                                     {"name", name.simplified()},
                                                                                     {"parts", QJsonArray::fromStringList(parts)}})
                                                               .toJson(QJsonDocument::Compact)),
                                .hlc = std::nullopt});
    return space;
}

void SharedSpace::record(const Library &library, const QString &space, const QString &name, const QString &founder) {
    ChangeLog::exec(library, "INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, ?, ?, ?, ?)",
                    {space, QString::fromLatin1(kind), name, founder, Spaces::lastHlc(library)});
}

std::expected<void, QString> SharedSpace::write(const Library &library, const QString &space, std::span<const ChangeLog::Entry> entries) {
    const Person *person = library.person();
    if (person == nullptr)
        return std::unexpected(u"Only a person writes a shared space, and this library has none (omabook identity says why)."_s);
    if (!isShared(library, space))
        return std::unexpected(u"This library is not a member of %1."_s.arg(space));
    const State now = state(library, space);
    if (const Role role = now.roleOf(person->id()); role != Role::Writer)
        return std::unexpected(role == Role::Reader ? u"You are a reader of %1: a reader can't write it."_s.arg(now.name)
                                                    : u"You are not a writer of %1 (yet: an invite counts once its inviter's sync "
                                                      "has brought it here)."_s.arg(now.name.isEmpty() ? space : now.name));
    const Library::Batch batch(library);
    for (ChangeLog::Entry entry : entries) {
        entry.space = space;
        entry.hlc = std::nullopt; // stamped now, after everything taken of the space
        ChangeLog::append(library, entry);
    }
    return {};
}

std::expected<void, QString> SharedSpace::setRole(const Library &library, const QString &space, const QString &person, Role role) {
    if (!Person::publicKeyFrom(person))
        return std::unexpected(u"%1 is not a person's id (omabook identity --export prints one)."_s.arg(person));
    const ChangeLog::Entry entry{.space = space,
                                 .entity = u"member"_s,
                                 .id = person,
                                 .field = u"role"_s,
                                 .value = role == Role::None ? u"removed"_s : roleName(role),
                                 .hlc = std::nullopt};
    return write(library, space, std::span(&entry, 1));
}

QString SharedSpace::memberAs(const Library &library, const State &state) {
    if (const Person *person = library.person(); person != nullptr && state.roleOf(person->id()) != Role::None)
        return person->id();
    QString reader;
    for (const QString &was : library.personFile().formerIds()) {
        const Role role = state.roleOf(was);
        if (role == Role::Writer)
            return was;
        if (role == Role::Reader && reader.isEmpty())
            reader = was;
    }
    return reader;
}

std::expected<bool, QString> SharedSpace::carryOver(const Library &library, const QString &space) {
    const Person *person = library.person();
    if (person == nullptr || !isShared(library, space))
        return false;
    const State now = state(library, space);
    if (now.roleOf(person->id()) == Role::Writer)
        return false;
    const QStringList formers = library.personFile().formerIds();
    const auto writer = std::ranges::find_if(formers, [&now](const QString &was) { return now.roleOf(was) == Role::Writer; });
    if (writer == formers.cend())
        return false;
    const auto was = library.personFile().former(*writer);
    if (!was)
        return std::unexpected(u"The person this computer was in %1 can't sign any more: %2"_s.arg(now.name, was.error()));
    const Library::Batch batch(library);
    ChangeLog::append(
        library,
        {.space = space, .entity = u"member"_s, .id = person->id(), .field = u"role"_s, .value = roleName(Role::Writer), .hlc = {}}, *was);
    return true;
}

SharedSpace::Taken SharedSpace::take(const Library &library, const QString &space, std::span<const SpaceOperation> incoming) {
    Taken taken;
    QList<SpaceOperation> all = held(library, space);
    QSet<QString> here;
    for (const SpaceOperation &op : std::as_const(all))
        here.insert(sameness(op));
    QSet<QString> arriving;
    for (const SpaceOperation &op : incoming) {
        if (here.contains(sameness(op))) {
            ++taken.known;
        } else if (!arriving.contains(sameness(op))) {
            arriving.insert(sameness(op));
            all << op;
        }
    }
    const State judged = admit(space, all);
    for (const Refused &refused : judged.refused)
        if (arriving.contains(sameness(refused.op)))
            taken.refused << refused;
    const Library::Batch batch(library);
    qint64 latest = 0;
    for (const SpaceOperation &op : judged.accepted)
        if (arriving.contains(sameness(op))) {
            keep(library, op);
            latest = std::max(latest, op.hlc);
            ++taken.added;
        }
    // What is written here next comes after what its writers wrote: the
    // latest of a field wins in the space as it does in the library.
    if (latest > 0)
        Spaces::observeHlc(library, latest);
    return taken;
}
