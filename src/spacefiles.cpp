#include "spacefiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>
#include <optional>
#include <ranges>
#include <span>

#include "changelog.h"
#include "formats.h"
#include "library.h"
#include "publishedlog.h"
#include "publishedspace.h"
#include "sharedkeys.h"
#include "sharedspace.h"
#include "spacefolder.h"
#include "spacelinks.h"
#include "spacereducer.h"
#include "spaces.h"
#include "spaceswitch.h"
#include "spaceworlds.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

using SpaceFiles::Carried;

constexpr auto filesPart = "files"_L1;
constexpr auto besideSuffix = ".omabook-files"_L1;
// The transports a space's files are fetched by (SpaceLinks::Role::Files).
constexpr auto byFolder = "folder"_L1;
constexpr auto byFile = "file"_L1;

[[nodiscard]] bool isShared(const Spaces::Recorded &space) { return space.kind == QLatin1StringView(SharedSpace::kind); }
[[nodiscard]] bool isPublished(const Spaces::Recorded &space) { return space.kind == QLatin1StringView(PublishedSpace::kind); }

// The files a space's book has, as the space says them: format, hash, size.
[[nodiscard]] QList<Carried> filesOf(const ReducedBook &book, const Spaces::Recorded &space) {
    QList<Carried> files;
    if (SpaceReducer::isRemoved(book.alive, book.removed))
        return files;
    for (const QJsonValue file : QJsonDocument::fromJson(book.fields.value(u"formats"_s).value.toUtf8()).array())
        if (const QString hash = file["hash"].toString(); !hash.isEmpty())
            files << Carried{
                .space = space.id, .name = space.name, .format = file["format"].toString(), .hash = hash, .size = file["size"].toInteger()};
    return files;
}

// Of the space's books, only those a plugin made whose file it gave the
// space with its world (#107): the space says the world with its file, and
// says the book is that plugin's (`generated`). A book a person made never
// travels so, whoever said its world.
[[nodiscard]] ReducedState withWorldFilesOnly(ReducedState state, const QList<SpaceOperation> &ops) {
    const QHash<QString, QString> same = SpaceReducer::sameAs(ops);
    QSet<QString> given;
    for (const SpaceWorlds::Declared &world : SpaceWorlds::declared(ops)) {
        const QString book = same.value(world.book, world.book);
        const QJsonObject identifiers =
            QJsonDocument::fromJson(state.books.value(book).fields.value(u"identifiers"_s).value.toUtf8()).object();
        if (world.file && !world.plugin.isEmpty() && identifiers.value("generated").toString() == world.plugin)
            given.insert(book);
    }
    state.books.removeIf([&given](const QMap<QString, ReducedBook>::iterator &book) { return !given.contains(book.key()); });
    return state;
}

// What the space says now, of a space this library is a member of or
// subscribes to: of every book when it lets its files travel, else of those
// whose plugin gave their file with their world; none of any other space.
[[nodiscard]] std::optional<ReducedState> sayingFiles(const Library &library, const Spaces::Recorded &space) {
    if (isShared(space)) {
        const SharedSpace::State state = SharedSpace::state(library, space.id);
        ReducedState reduced = SpaceReducer::reduce(state.accepted);
        return state.files ? reduced : withWorldFilesOnly(std::move(reduced), state.accepted);
    }
    if (isPublished(space) && SubscribedSpace::isSubscribed(library, space.id))
        return SpaceReducer::reduce(SubscribedSpace::operations(library, space.id));
    return std::nullopt;
}

[[nodiscard]] std::optional<Spaces::Recorded> recordedAs(const Library &library, const QString &space) {
    for (const Spaces::Recorded &recorded : Spaces::recorded(library))
        if (recorded.id == space)
            return recorded;
    return std::nullopt;
}

[[nodiscard]] std::expected<Spaces::Recorded, QString> resolve(const Library &library, const QString &which) {
    return SpaceSwitch::resolve(library, which).transform_error([](const Failure &why) { return why.text(); });
}

[[nodiscard]] std::expected<SpaceFiles::Switched, QString> setShared(const Library &library, const Spaces::Recorded &space, bool on) {
    SpaceFiles::Switched switched{.space = space.id, .kind = space.kind, .name = space.name, .on = on, .changed = false};
    if (SharedSpace::state(library, space.id).files == on)
        return switched;
    const ChangeLog::Entry entry{
        .space = space.id, .entity = u"space"_s, .id = space.id, .field = u"files"_s, .value = on ? u"1"_s : u"0"_s, .hlc = {}};
    if (const auto written = SharedSpace::write(library, space.id, std::span(&entry, 1)); !written)
        return std::unexpected(written.error());
    switched.changed = true;
    return switched;
}

[[nodiscard]] std::expected<SpaceFiles::Switched, QString> setPublished(const Library &library, const Spaces::Recorded &space, bool on) {
    std::optional<PublishedLog::Record> record = PublishedLog::recorded(library, space.id);
    if (SubscribedSpace::isSubscribed(library, space.id) || !record)
        return std::unexpected(u"Only the owner of %1 lets its files travel, and you follow it."_s.arg(space.name));
    SpaceFiles::Switched switched{.space = space.id, .kind = space.kind, .name = space.name, .on = on, .changed = false};
    if (record->parts.contains(filesPart) == on)
        return switched;
    if (on)
        record->parts << filesPart;
    else
        record->parts.removeAll(filesPart);
    PublishedLog::remember(library, space.id, *record);
    if (const auto followed = PublishedSpace::follow(library, space.id); !followed)
        return std::unexpected(followed.error());
    switched.changed = true;
    return switched;
}

// The newest key of the space held here, which seals what it writes now.
[[nodiscard]] const SpaceKey &newest(const SharedSpace::State &state, std::span<const SpaceKey> keys) {
    for (const KeyRotation &rotation : state.rotations | std::views::reverse)
        if (const auto key = std::ranges::find_if(keys, [&rotation](const SpaceKey &one) { return one.id() == rotation.key; });
            key != keys.end())
            return *key;
    return keys.front();
}

// Where this library keeps the file of `hash`, when it has it.
[[nodiscard]] std::optional<FormatFile> fileHere(const Library &library, const QString &hash) {
    const qint64 book = library.bookWithHash(hash);
    for (const FormatFile &file : book > 0 ? library.formatFiles(book) : QList<FormatFile>{})
        if (file.hash == hash)
            return FormatFile{.format = file.format, .path = library.formatPath(book, file.format), .hash = hash, .size = file.size};
    return std::nullopt;
}

// Every file the space's books have, as it says them.
[[nodiscard]] QList<Carried> everyFile(const ReducedState &state, const Spaces::Recorded &space) {
    QList<Carried> files;
    for (const ReducedBook &book : state.books)
        files += filesOf(book, space);
    return files;
}

// The file `file` from the shared space's folder, into `into`.
[[nodiscard]] std::expected<void, QString> fromFolder(const Library &library, const Carried &file, const QString &into) {
    const auto folder = SpaceLinks::of(library, file.space, SpaceLinks::Role::Files, byFolder);
    if (!folder)
        return std::unexpected(u"Sync %1 through its folder first (omabook spaces sync): its files come from there."_s.arg(file.name));
    const auto keys = SharedKeys::held(library, file.space);
    if (!keys)
        return std::unexpected(keys.error());
    return SpaceFolder::readFile(folder->address, file.space, *keys, file.hash, into);
}

// The file `file` from beside the space's file a subscription was taken from.
[[nodiscard]] std::expected<QString, QString> fromBeside(const Library &library, const Carried &file) {
    const auto taken = SpaceLinks::of(library, file.space, SpaceLinks::Role::Files, byFile);
    if (!taken) {
        const bool link = SpaceLinks::of(library, file.space, SpaceLinks::Role::Followed).has_value();
        return std::unexpected(link ? u"%1 is followed by a link, and a gist carries no book files: ask its owner for its %2 file, with "
                                      "its files beside it, and import that."_s.arg(file.name, QLatin1StringView(SpaceSnapshotFile::suffix))
                                    : u"Import %1 again from its %2 file, with its files beside it: they come from there."_s.arg(
                                          file.name, QLatin1StringView(SpaceSnapshotFile::suffix)));
    }
    QString path = QDir(SpaceFiles::besideOf(taken->address)).filePath(file.hash + u'.' + file.format);
    if (!QFileInfo::exists(path))
        return std::unexpected(u"%1 is not beside %2, where the space's files are."_s.arg(QFileInfo(path).fileName(), taken->address));
    if (Library::hashFile(path) != file.hash)
        return std::unexpected(u"%1 is not the file the space names: its hash differs."_s.arg(path));
    return path;
}

[[nodiscard]] std::expected<void, QString> fetchOne(Library &library, qint64 book, const Carried &file) {
    const QTemporaryDir scratch;
    QString path = scratch.filePath(u"book."_s + file.format);
    const auto record = recordedAs(library, file.space);
    if (record && isShared(*record)) {
        if (const auto got = fromFolder(library, file, path); !got)
            return std::unexpected(got.error());
    } else if (const auto beside = fromBeside(library, file); beside) {
        path = *beside;
    } else {
        return std::unexpected(beside.error());
    }
    if (QString error; !library.addFormat(book, path, &error))
        return std::unexpected(error);
    // What was given to the book is what was checked, or the library has it no longer.
    const QList<FormatFile> held = library.formatFiles(book);
    if (std::ranges::none_of(held, [&file](const FormatFile &one) { return one.hash == file.hash; }))
        return std::unexpected(u"The file came, but the book here holds another."_s);
    return {};
}

} // namespace

bool SpaceFiles::on(const Library &library, const QString &space) {
    const auto record = recordedAs(library, space);
    if (!record)
        return false;
    if (isShared(*record))
        return SharedSpace::state(library, space).files;
    if (SubscribedSpace::isSubscribed(library, space)) {
        // Its owner's switch, as the files of books other than those a plugin gave with their worlds say it.
        const QList<SpaceOperation> ops = SubscribedSpace::operations(library, space);
        const ReducedState state = SpaceReducer::reduce(ops);
        return everyFile(state, *record).size() > everyFile(withWorldFilesOnly(state, ops), *record).size();
    }
    const auto published = PublishedLog::recorded(library, space);
    return published && published->parts.contains(filesPart);
}

std::expected<SpaceFiles::Switched, QString> SpaceFiles::set(const Library &library, const QString &which, bool on) {
    const auto space = resolve(library, which);
    if (!space)
        return std::unexpected(space.error());
    return isShared(*space) ? setShared(library, *space, on) : setPublished(library, *space, on);
}

QString SpaceFiles::said(const Switched &switched) {
    if (!switched.on)
        return u"The books' files of %1 no longer travel: nothing more of them goes, and what a member fetched before stays theirs."_s.arg(
            switched.name);
    return switched.kind == QLatin1StringView(SharedSpace::kind)
               ? u"The books' files of %1 travel now, to every member of it, and to whoever is made one later: each member who has "
                 "a book's file puts it in the space's folder at their next sync, sealed with its key, and the others fetch it "
                 "when they open the book."_s.arg(switched.name)
               : u"The books' files of %1 travel now, to whoever has its file or link: everyone you give it to, and everyone they "
                 "give it to. They go beside its %2 file when you write it (never by a gist), and those who subscribe fetch each "
                 "when they open the book."_s.arg(switched.name, QLatin1StringView(SpaceSnapshotFile::suffix));
}

QList<Carried> SpaceFiles::of(const Library &library, qint64 book) {
    const auto here = library.book(book);
    if (!here || !here->formats.isEmpty())
        return {};
    QList<Carried> files;
    for (const Spaces::Recorded &space : Spaces::recorded(library)) {
        const QString gid = SubscribedSpace::bookIn(library, space.id, book);
        if (gid.isEmpty())
            continue;
        if (const auto state = sayingFiles(library, space))
            files += filesOf(state->books.value(gid), space);
    }
    std::ranges::stable_partition(files, [](const Carried &file) { return Formats::readable(file.format); });
    return files;
}

std::expected<Carried, QString> SpaceFiles::fetch(Library &library, qint64 book) {
    const auto here = library.book(book);
    if (!here)
        return std::unexpected(u"There is no book %1 here."_s.arg(book));
    if (!here->formats.isEmpty())
        return std::unexpected(u"%1 has a file of its own here, which stays as it is."_s.arg(here->meta.title));
    const QList<Carried> files = of(library, book);
    if (files.isEmpty())
        return std::unexpected(u"No space you are in lets the file of %1 travel."_s.arg(here->meta.title));
    QStringList problems;
    for (const Carried &file : files) {
        const auto fetched = fetchOne(library, book, file);
        if (fetched)
            return file;
        if (!problems.contains(fetched.error()))
            problems << fetched.error();
    }
    return std::unexpected(problems.join(u' '));
}

qsizetype SpaceFiles::bringWorldFiles(Library &library, const QString &space) {
    const auto record = recordedAs(library, space);
    if (!record || !(isShared(*record) || SubscribedSpace::isSubscribed(library, space)))
        return 0;
    const QList<SpaceOperation> ops =
        isShared(*record) ? SharedSpace::state(library, space).accepted : SubscribedSpace::operations(library, space);
    const ReducedState given = withWorldFilesOnly(SpaceReducer::reduce(ops), ops);
    qsizetype brought = 0;
    for (auto book = given.books.cbegin(); book != given.books.cend(); ++book) {
        const qint64 here = SubscribedSpace::bookHere(library, space, book.key());
        const auto held = here > 0 ? library.book(here) : std::nullopt;
        if (!held || !held->formats.isEmpty())
            continue;
        const QList<Carried> files = filesOf(*book, *record);
        brought += std::ranges::any_of(files, [&](const Carried &file) { return fetchOne(library, here, file).has_value(); }) ? 1 : 0;
    }
    return brought;
}

std::expected<qsizetype, QString> SpaceFiles::send(const Library &library, const QString &space, const QString &folder) {
    SpaceLinks::remember(library, SpaceLinks::Role::Files,
                         {.space = space, .transport = QString(byFolder), .address = QFileInfo(folder).absoluteFilePath()});
    const auto record = recordedAs(library, space);
    const auto saying = record && isShared(*record) ? sayingFiles(library, *record) : std::nullopt;
    const QList<Carried> files = saying ? everyFile(*saying, *record) : QList<Carried>{};
    if (files.isEmpty())
        return 0;
    const auto keys = SharedKeys::held(library, space);
    if (!keys)
        return std::unexpected(keys.error());
    const SharedSpace::State state = SharedSpace::state(library, space);
    const SpaceKey &sealing = newest(state, *keys);
    qsizetype written = 0;
    for (const Carried &file : files) {
        const auto mine = fileHere(library, file.hash);
        if (!mine)
            continue;
        const auto sent = SpaceFolder::writeFile(folder, space, *keys, sealing, {file.hash, mine->path});
        if (!sent)
            return std::unexpected(sent.error());
        written += *sent ? 1 : 0;
    }
    return written;
}

QString SpaceFiles::besideOf(const QString &snapshotFile) {
    const QFileInfo file(snapshotFile);
    const QLatin1StringView suffix(SpaceSnapshotFile::suffix);
    QString name = file.fileName();
    if (name.endsWith(suffix))
        name.chop(suffix.size());
    return file.dir().absoluteFilePath(name + besideSuffix);
}

std::expected<qsizetype, QString> SpaceFiles::writeBeside(const Library &library, const SpaceSnapshot &snapshot,
                                                          const QString &snapshotFile) {
    const Spaces::Recorded space{.id = snapshot.space, .kind = snapshot.kind, .name = snapshot.name, .owner = snapshot.owner.person};
    ReducedState state = SpaceReducer::reduce(snapshot.operations);
    if (!snapshot.parts.contains(filesPart))
        state = withWorldFilesOnly(std::move(state), snapshot.operations);
    const QList<Carried> files = everyFile(state, space);
    if (files.isEmpty())
        return 0;
    const QDir beside(besideOf(snapshotFile));
    if (!beside.mkpath(u"."_s))
        return std::unexpected(u"Can't make the folder %1 for the books' files."_s.arg(beside.path()));
    qsizetype copied = 0;
    for (const Carried &file : files) {
        const QString target = beside.filePath(file.hash + u'.' + file.format);
        const auto mine = fileHere(library, file.hash);
        if (!mine || QFileInfo::exists(target))
            continue;
        // Whole or not at all, under its own name only once it is.
        const QString partial = target + u".part"_s;
        QFile::remove(partial);
        if (!QFile::copy(mine->path, partial) || !QFile::rename(partial, target))
            return std::unexpected(u"Can't copy %1 beside the space's file, to %2."_s.arg(mine->path, beside.path()));
        ++copied;
    }
    return copied;
}

void SpaceFiles::takenFrom(const Library &library, const QString &space, const QString &snapshotFile) {
    const QString path = snapshotFile.startsWith("file:"_L1) ? QUrl(snapshotFile).toLocalFile() : snapshotFile;
    SpaceLinks::remember(library, SpaceLinks::Role::Files,
                         {.space = space, .transport = QString(byFile), .address = QFileInfo(path).absoluteFilePath()});
}
