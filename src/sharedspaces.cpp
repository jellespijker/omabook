#include "sharedspaces.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>

#include "library.h"
#include "membernames.h"
#include "person.h"
#include "sharedcollection.h"
#include "sharedkeys.h"
#include "spacefiles.h"
#include "spacefolder.h"
#include "spaceinvite.h"
#include "spacekey.h"
#include "spacekeys.h"
#include "spaces.h"
#include "subscribedspace.h"
#include "untrustedtext.h"

using namespace Qt::StringLiterals;

namespace {

using SharedSpaces::Member;
using SharedSpaces::Summary;

[[nodiscard]] QString collectionPath(const Library &library, const QString &space) {
    const auto collection = library.collection(SubscribedSpace::linkedCollection(library, space));
    return collection ? collection->path : QString();
}

[[nodiscard]] Summary sharedSummary(const Library &library, const QString &space, const QString &recordedName) {
    const SharedSpace::State state = SharedSpace::state(library, space);
    // As a person given up at a pairing, where that is who this library is in the space (#50).
    const QString me = SharedSpace::memberAs(library, state);
    Summary summary{.space = space,
                    .kind = QString::fromLatin1(SharedSpace::kind),
                    .name = state.name.isEmpty() ? recordedName : state.name,
                    .role = me.isEmpty() ? QString() : SharedSpace::roleName(state.roleOf(me)),
                    .members = {},
                    .collection = collectionPath(library, space),
                    .files = state.files};
    const QHash<QString, QString> names = MemberNames::of(state.accepted);
    for (const SharedSpace::Member &member : state.members) {
        QString name = names.value(member.person);
        if (library.personFile().isMine(member.person))
            name = name.isEmpty() ? u"you"_s : u"you, %1"_s.arg(name);
        summary.members << Member{.person = member.person, .name = name, .role = SharedSpace::roleName(member.role)};
    }
    return summary;
}

[[nodiscard]] Summary personalSummary(const Library &library) {
    Summary summary{.space = Spaces::personalSpace(library),
                    .kind = u"personal"_s,
                    .name = u"Personal"_s,
                    .role = u"writer"_s,
                    .members = {},
                    .collection = {},
                    .files = false};
    for (const Spaces::Computer &computer : Spaces::computers(library))
        summary.members << Member{.person = computer.device,
                                  .name = computer.device == Spaces::deviceId(library) ? u"this computer"_s : computer.name,
                                  .role = u"writer"_s};
    return summary;
}

[[nodiscard]] Summary publishedSummary(const Library &library, const Spaces::Recorded &space) {
    const bool subscribed = SubscribedSpace::isSubscribed(library, space.id);
    return {.space = space.id,
            .kind = space.kind,
            .name = space.name,
            .role = subscribed ? u"reader"_s : u"writer"_s,
            .members = {Member{.person = space.owner, .name = subscribed ? QString() : u"you"_s, .role = u"writer"_s}},
            .collection = subscribed ? collectionPath(library, space.id) : QString(),
            .files = SpaceFiles::on(library, space.id)};
}

[[nodiscard]] std::expected<QString, QString> writeInvite(const QString &out, const SpaceInvite &invite, const QByteArray &bytes) {
    QString path = out.isEmpty() ? QDir::currentPath() : out;
    if (QFileInfo(path).isDir()) {
        const QString name = invite.name.simplified().replace(u'/', u'-');
        path = QDir(path).absoluteFilePath(u"%1 for %2%3"_s.arg(name, invite.to.left(8), QLatin1StringView(SpaceInvites::suffix)));
    }
    // What it holds is sealed for its reader alone, so it is an ordinary file: it is made to be sent.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0 || !file.commit())
        return std::unexpected(u"Can't write the invite %1: %2"_s.arg(path, file.errorString()));
    return QFileInfo(path).absoluteFilePath();
}

// Of what this computer wrote in the space, what the folder doesn't hold yet.
[[nodiscard]] QList<SpaceOperation> unsent(const Library &library, const QString &space, const QString &folder) {
    const QString device = Spaces::deviceId(library);
    const qint64 sent = SpaceFolder::lastWritten(folder, space, device);
    QList<SpaceOperation> mine = SharedSpace::held(library, space);
    mine.removeIf([&](const SpaceOperation &op) { return op.device != device || op.seq <= sent; });
    std::ranges::sort(mine, {}, &SpaceOperation::seq);
    return mine;
}

// The invite in `bytes`, opened by `me`, or by a person this library was
// before a pairing: one sealed to who it was then is still for it (#50).
[[nodiscard]] std::expected<SpaceInvites::Opened, QString> openInvite(const Library &library, const Person &me, const QByteArray &bytes) {
    auto opened = SpaceInvites::open(me, bytes);
    for (const QString &was : library.personFile().formerIds()) {
        if (opened)
            break;
        if (const auto former = library.personFile().former(was))
            if (auto byFormer = SpaceInvites::open(*former, bytes))
                opened = std::move(byFormer);
    }
    return opened;
}

[[nodiscard]] QString refusal(const SharedSpace::Refused &refused) {
    return u"operation %1 of %2 (%3 %4): %5"_s.arg(refused.op.seq).arg(refused.op.device, refused.op.entity, refused.op.field, refused.why);
}

// Changes the key again when the newest was handed to someone who is not a
// member now, or not to this writer (SharedKeys::needsRotating).
[[nodiscard]] std::expected<void, QString> rotateIfNeeded(const Library &library, const QString &space) {
    const auto keys = SharedKeys::held(library, space);
    if (!keys)
        return std::unexpected(keys.error());
    if (!SharedKeys::needsRotating(library, SharedSpace::state(library, space), *keys))
        return {};
    return SharedKeys::rotate(library, space);
}

// Takes in what is new in the folder. A batch sealed with a key handed over
// in another batch opens once that one is taken: so again, with the keys
// learnt, until no key is new (one round per change of the key, at most).
[[nodiscard]] std::expected<void, QString> takeFrom(const Library &library, const QString &space, const QString &folder,
                                                    SharedSpaces::Synced &synced) {
    QStringList refusedOps; // of every round; of the batches, only the last round's say what didn't open
    for (;;) {
        const auto keys = SharedKeys::held(library, space);
        if (!keys)
            return std::unexpected(keys.error());
        const SpaceFolder::Read read = SpaceFolder::read(folder, space, *keys, SharedSpace::lastHeld(library, space));
        synced.batches += read.batches;
        const SharedSpace::Taken taken = SharedSpace::take(library, space, read.ops);
        synced.taken += taken.added;
        synced.known += taken.known;
        for (const SharedSpace::Refused &refused : taken.refused)
            if (const QString said = refusal(refused); !refusedOps.contains(said))
                refusedOps << said;
        const auto learnt = SharedKeys::learn(library, space);
        if (!learnt)
            return std::unexpected(learnt.error());
        if (*learnt == 0 || read.batches == 0) {
            synced.refused = read.refused + refusedOps;
            return {};
        }
    }
}

// Writes what this computer wrote of the space and the folder lacks, each
// run of operations sealed with the same key as one batch (SharedKeys::sealing).
[[nodiscard]] std::expected<qsizetype, QString> sendTo(const Library &library, const QString &space, const QString &folder) {
    const QList<SpaceOperation> mine = unsent(library, space, folder);
    if (mine.isEmpty())
        return 0;
    const auto keys = SharedKeys::held(library, space);
    if (!keys)
        return std::unexpected(keys.error());
    const SharedSpace::State state = SharedSpace::state(library, space);
    for (qsizetype from = 0; from < mine.size();) {
        const SpaceKey &key = SharedKeys::sealing(state, *keys, mine.at(from));
        qsizetype to = from + 1;
        while (to < mine.size() && &SharedKeys::sealing(state, *keys, mine.at(to)) == &key)
            ++to;
        if (const auto sent = SpaceFolder::write(folder, space, key, Spaces::deviceId(library), mine.sliced(from, to - from)); !sent)
            return std::unexpected(sent.error());
        from = to;
    }
    return mine.size();
}

} // namespace

QList<Summary> SharedSpaces::list(const Library &library) {
    QList<Summary> spaces{personalSummary(library)};
    for (const Spaces::Recorded &space : Spaces::recorded(library)) {
        if (space.kind == QLatin1StringView(SharedSpace::kind))
            spaces << sharedSummary(library, space.id, space.name);
        else if (space.kind == "published"_L1)
            spaces << publishedSummary(library, space);
    }
    return spaces;
}

QString SharedSpaces::keyFile(const Library &library, const QString &space) {
    return SpaceKey::fileFor(SharedKeys::folder(library), space);
}

std::expected<QString, QString> SharedSpaces::resolve(const Library &library, const QString &which) {
    QStringList meant;
    for (const Spaces::Recorded &space : Spaces::recorded(library))
        if (space.kind == QLatin1StringView(SharedSpace::kind)
            && (space.id == which || space.name.compare(which.simplified(), Qt::CaseInsensitive) == 0))
            meant << space.id;
    if (meant.isEmpty())
        return std::unexpected(u"This library is in no shared space called %1 (omabook spaces lists them)."_s.arg(which));
    if (meant.size() > 1)
        return std::unexpected(u"More than one shared space goes by %1; name one by its id: %2"_s.arg(which, meant.join(", "_L1)));
    return meant.first();
}

std::expected<SharedSpaces::Created, QString> SharedSpaces::create(Library &library, const QString &name, const QStringList &parts,
                                                                   const QString &collection, const SpaceWorlds::Ask &worlds) {
    const QString path = collection.trimmed().isEmpty() ? name.simplified().replace(u'/', u'-') : collection.trimmed();
    auto key = SpaceKey::generate();
    if (!key)
        return std::unexpected(key.error());
    const Library::Batch batch(library);
    const auto existing = library.collectionNamed(path);
    const auto linked = existing ? std::expected<qint64, QString>(existing->id) : library.addCollection(path);
    if (!linked)
        return std::unexpected(linked.error());
    const auto space = SharedSpace::found(library, name, parts);
    if (!space)
        return std::unexpected(space.error());
    if (const auto kept = key->keepIn(keyFile(library, *space)); !kept)
        return std::unexpected(kept.error());
    SubscribedSpace::link(library, *space, *linked);
    QList<qint64> books;
    if (const auto shelf = library.collection(*linked))
        for (const Book &book : shelf->of(library.books()))
            books << book.id;
    SpaceWorlds::ask(library, {.space = *space, .kind = QString::fromLatin1(SharedSpace::kind), .books = books, .parts = parts}, worlds);
    const auto written = SharedCollection::contribute(library, *space, {});
    if (!written)
        return std::unexpected(written.error());
    return Created{.space = sharedSummary(library, *space, name), .newCollection = !existing, .operations = *written};
}

std::expected<SharedSpaces::Invited, QString> SharedSpaces::invite(Library &library, const QString &which, const QString &person,
                                                                   SharedSpace::Role role, const QString &out) {
    const auto space = resolve(library, which);
    if (!space)
        return std::unexpected(space.error());
    const Person *me = library.person();
    if (me == nullptr)
        return std::unexpected(u"Only a person invites, and this library has none (omabook identity says why)."_s);
    const auto keys = SharedKeys::held(library, *space);
    if (!keys)
        return std::unexpected(keys.error());
    if (const auto added = SharedSpace::setRole(library, *space, person.trimmed(), role); !added)
        return std::unexpected(added.error());
    const Summary summary = sharedSummary(library, *space, which);
    const SpaceInvite invite{
        .space = *space, .name = summary.name, .role = SharedSpace::roleName(role), .from = me->id(), .to = person.trimmed()};
    const auto bytes = SpaceInvites::write(*me, invite, *keys);
    if (!bytes)
        return std::unexpected(bytes.error());
    const auto file = writeInvite(out, invite, *bytes);
    if (!file)
        return std::unexpected(file.error());
    return Invited{.space = summary, .person = invite.to, .role = invite.role, .file = *file};
}

std::expected<Summary, QString> SharedSpaces::remove(const Library &library, const QString &which, const QString &person) {
    const auto space = resolve(library, which);
    if (!space)
        return std::unexpected(space.error());
    if (SharedSpace::state(library, *space).roleOf(person.trimmed()) == SharedSpace::Role::None)
        return std::unexpected(u"%1 is not a member of %2."_s.arg(person, which));
    if (const auto removed = SharedSpace::setRole(library, *space, person.trimmed(), SharedSpace::Role::None); !removed)
        return std::unexpected(removed.error());
    // What is sealed from now on, they can't open; and what they write past what is held here now is refused.
    if (const auto rotated = SharedKeys::rotate(library, *space); !rotated)
        return std::unexpected(rotated.error());
    return sharedSummary(library, *space, which);
}

std::expected<Summary, QString> SharedSpaces::join(Library &library, const QString &file, const QString &as) {
    const Person *me = library.person();
    if (me == nullptr)
        return std::unexpected(u"Only a person joins a space, and this library has none (omabook identity says why)."_s);
    QFile read(file);
    if (!read.open(QIODevice::ReadOnly))
        return std::unexpected(u"Can't read the invite %1: %2"_s.arg(file, read.errorString()));
    const auto opened = openInvite(library, *me, read.readAll());
    if (!opened)
        return std::unexpected(opened.error());
    const QString &space = opened->invite.space;
    if (const auto kept = SpaceKeys::keepAll(SharedKeys::folder(library), space, opened->keys); !kept)
        return std::unexpected(kept.error());
    const Library::Batch batch(library);
    SharedSpace::record(library, space, opened->invite.name);
    if (const auto filed = SharedCollection::file(library, space, SharedSpace::state(library, space).accepted, as); !filed)
        return std::unexpected(filed.error());
    if (const auto carried = SharedSpace::carryOver(library, space); !carried)
        return std::unexpected(carried.error());
    Summary summary = sharedSummary(library, space, opened->invite.name);
    // Until the inviter's sync brings the operation that made them a member, the invite says what they are.
    if (summary.role.isEmpty())
        summary.role = opened->invite.role;
    return summary;
}

std::expected<Summary, Failure> SharedSpaces::goBy(const Library &library, const QString &which, const QString &name) {
    const auto space = resolve(library, which);
    if (!space)
        return std::unexpected(Failure::said(space.error()));
    const std::optional<QString> chosen = UntrustedText::name(name);
    if (!chosen)
        return std::unexpected(Failure(Failure::Code::NotAName, {QString::number(UntrustedText::maxNameLength)}));
    // Written as the library's person, whose role the space's rules then judge (SharedSpace::write).
    const Person *me = library.person();
    const ChangeLog::Entry entry = MemberNames::entry(*space, me != nullptr ? me->id() : QString(), *chosen);
    if (const auto written = SharedSpace::write(library, *space, std::span(&entry, 1)); !written)
        return std::unexpected(Failure::said(written.error()));
    return sharedSummary(library, *space, which);
}

std::expected<SharedSpaces::Synced, QString> SharedSpaces::sync(Library &library, const QString &which, const QString &folder) {
    const auto space = resolve(library, which);
    if (!space)
        return std::unexpected(space.error());
    if (!QFileInfo(folder).isDir())
        return std::unexpected(u"There is no folder %1 to sync through."_s.arg(folder));
    Synced synced;
    const QList<SpaceOperation> before = SharedSpace::state(library, *space).accepted;
    if (const auto taken = takeFrom(library, *space, folder, synced); !taken)
        return std::unexpected(taken.error());
    // A membership of a person given up at a pairing, handed to the one now, once it is here (#50).
    if (const auto carried = SharedSpace::carryOver(library, *space); !carried)
        return std::unexpected(carried.error());
    auto filed = SharedCollection::file(library, *space, before);
    if (!filed)
        return std::unexpected(filed.error());
    synced.filed = *filed;
    const Person *me = library.person();
    if (me != nullptr && SharedSpace::state(library, *space).roleOf(me->id()) == SharedSpace::Role::Writer) {
        const auto written = SharedCollection::contribute(library, *space, *filed);
        if (!written)
            return std::unexpected(written.error());
        synced.written = *written;
    }
    if (const auto healed = rotateIfNeeded(library, *space); !healed)
        return std::unexpected(healed.error());
    const auto sent = sendTo(library, *space, folder);
    if (!sent)
        return std::unexpected(sent.error());
    synced.sent = *sent;
    const auto files = SpaceFiles::send(library, *space, folder);
    if (!files)
        return std::unexpected(files.error());
    synced.files = *files;
    // Once the folder is known as where the space's files are (send remembers it).
    synced.fetched = SpaceFiles::bringWorldFiles(library, *space);
    synced.space = sharedSummary(library, *space, which);
    return synced;
}
