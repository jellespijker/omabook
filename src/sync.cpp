#include "sync.h"

#include "log.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSet>
#include <QSysInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

#include "collections.h"
#include "formats.h"
#include "graphsync.h"
#include "hlc.h"
#include "olderpeer.h"
#include "pluginfilesync.h"
#include "replication.h"
#include "spaces.h"
#include "syncguard.h"
#include "syncidentity.h"
#include "synclog.h"
#include "syncmanifest.h"
#include "synctake.h"

namespace {

// What names a book of the other side's manifest that has no file: its keys,
// as an Omabook that speaks the log gives them, or else made from its details.
QStringList entryKeysOf(const QJsonObject &theirs) {
    if (theirs.contains("keys")) {
        QStringList keys;
        for (const auto &key : theirs.value("keys").toArray())
            keys << key.toString();
        return keys;
    }
    return Library::entryKeys(theirs.value("identifiers").toObject(), theirs.value("isbn").toString(), theirs.value("title").toString(),
                              BookMeta::fromJson(theirs).authors);
}

// Why a book in a plugin's format was refused by an Omabook where nothing
// reads it: one from before it kept such a book for the plugin it needs, or
// one that doesn't know that plugin.
QString notReadThere(const QString &format) {
    if (const std::optional<PluginFormat> plugin = Formats::pluginFormat(format))
        return QString("there, nothing reads .%1 files, and Omabook there is too old to keep the book until it does: install the %2 "
                       "plugin there (omabook plugin add %3), or update Omabook there")
            .arg(format, plugin->title, plugin->plugin);
    return QString("there, nothing reads .%1 files; install the plugin that made it there").arg(format);
}

// Counts one book in a summary, with its title for a terminal.
void note(int &count, QStringList &titles, const QString &title) {
    ++count;
    titles << title;
}

void syncCollectionsAndGraph(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary) {
    if (manifest.contains("collections")) {
        summary.collectionsTaken = Collections::take(library, manifest.value("collections").toArray());
        if (const QJsonArray mine = Collections::manifest(library); !mine.isEmpty()) {
            if (const auto sent = remote.handOver(QJsonDocument(mine), {"collections", "take", "--json"}))
                summary.collectionsSent = *sent;
            else
                summary.problems << "The collections could not be sent to " + remote.host() + ".";
        }
    }
    if (manifest.contains("graph")) {
        summary.graphTaken = GraphSync::take(library, manifest.value("graph").toObject());
        if (const auto sent = remote.handOver(QJsonDocument(GraphSync::bundle(library)), {"graph", "take", "--json"}))
            summary.graphSent = *sent;
        else
            summary.problems << "The knowledge graph could not be sent to " + remote.host() + ": " + sent.error();
    }
}

void logSyncSummary(const SyncSummary &summary, const QString &host) {
    qCInfo(lcSync).noquote() << (summary.dryRun ? "dry run with" : "with") << host << ":" << summary.received << "received,"
                             << summary.sent << "sent," << summary.placesTaken + summary.placesSent << "reading places,"
                             << summary.detailsTaken << "details taken," << summary.detailsSent << "details sent,"
                             << summary.detailsUndecided << "details undecided," << summary.removedHere << "removed here,"
                             << summary.removedThere << "removed there," << summary.graphTaken << "graph taken,"
                             << summary.graphSent << "graph sent," << summary.problems.size() << "problems";
}

// Why the two libraries may not sync, as each side knows the other (#209):
// before anything is written on either. Their manifest is checked here; this
// library's device and name there, where an Omabook from before that check
// can't answer, and isn't refused for it.
[[nodiscard]] QString identityRefused(const Library &library, const Remote &remote, const QJsonObject &manifest) {
    if (manifest.value("log").toInt() < 1)
        return {};
    if (const auto here = SyncIdentity::check(library, manifest, remote.host()); !here)
        return here.error().text();
    const QJsonObject mine{{"device", Spaces::deviceId(library)}, {"name", QSysInfo::machineHostName()}};
    const auto there = remote.exchange(QJsonDocument(mine), {"sync", "--identity"});
    const QString refused = there ? there->value("refused").toString() : QString();
    return refused.isEmpty() ? refused : remote.host() + " refuses to sync with this computer: " + refused;
}

// What the other side holds, or none, with why in `summary`. An Omabook that
// speaks the log answers `manifest --log` with a manifest that says so and
// leaves out what the log carries; one from before refuses the option it
// doesn't know (exit code 1), and is asked for the manifest it has always given.
std::optional<QJsonObject> theirManifest(const Library &library, const Remote &remote, SyncSummary &summary) {
    Remote::Result listed = remote.omabook({"manifest", "--log"});
    if (listed.code == 1)
        listed = remote.omabook({"manifest"});
    QJsonObject manifest = QJsonDocument::fromJson(listed.output).object();
    if (!listed.ok() || !manifest.contains("books")) {
        summary.error = listed.code == 127 || listed.error.contains("not found")
                            ? "Omabook isn't installed on " + remote.host() + ", or isn't on its PATH."
                            : listed.error.isEmpty() ? remote.host() + " can't be reached over ssh."
                                                     : remote.host() + ": " + listed.error.section('\n', -1);
        return std::nullopt;
    }
    // A manifest in a newer shape than this Omabook knows could be misread,
    // and a sync acts on what it reads; better to say which side to update.
    if (manifest.value("version").toInt(1) > manifestVersion) {
        summary.error = QString("%1 runs a newer Omabook (%2) than this computer (%3), and the two can't sync until this one "
                                "is updated.")
                            .arg(remote.host(), manifest.value("omabook").toString("unknown"), QStringLiteral(OMABOOK_VERSION));
        return std::nullopt;
    }
    // Who they are, before anything is written on either side (#209).
    if (summary.error = identityRefused(library, remote, manifest); !summary.error.isEmpty())
        return std::nullopt;
    return manifest;
}

// What their files of a book name here: the book that has one of them,
// whether one of them was removed here, and their hashes, each also added to
// `theirHashes`. A file they wrote details into (#74) is still known by the
// hash it came in with.
struct Named {
    qint64 mine = 0;
    bool removedHere = false;
    QStringList hashes;
};

[[nodiscard]] Named namedHere(const Library &library, const QJsonArray &files, QSet<QString> &theirHashes) {
    Named named;
    for (const auto &value : files) {
        const QJsonObject file = value.toObject();
        const QString hash = file.value("hash").toString();
        named.hashes << hash;
        named.removedHere = named.removedHere || library.wasRemoved(hash);
        for (const QString &known : {hash, file.value("original").toString()}) {
            if (known.isEmpty())
                continue;
            theirHashes << known;
            named.mine = named.mine ? named.mine : library.bookWithHash(known);
        }
    }
    return named;
}

// A book without a file taken from there, which keeps the global id it has
// there: it is the same book, paired from the start, and never again by its
// identifiers (#158). An id a book here has already stays that book's.
[[nodiscard]] Library::Added addTheirs(Library &library, const BookMeta &meta, const QJsonObject &theirs) {
    const Library::Batch batch(library);
    const Library::Added added = library.addEntry(meta, theirs.value("paper").toBool());
    const QString gid = theirs.value("gid").toString();
    if (added.error.isEmpty() && !gid.isEmpty() && Spaces::localId(library, QStringLiteral("book"), gid) == 0)
        Spaces::setGlobalId(library, QStringLiteral("book"), added.id, gid);
    return added;
}

// Books without a file are known by their entry keys (Library::entryKeys): every book here by its keys.
[[nodiscard]] QHash<QString, qint64> keysHere(const Library &library) {
    QHash<QString, qint64> keys;
    for (const Book &book : library.books())
        for (const QString &key : Library::entryKeys(book))
            keys.insert(key, book.id);
    return keys;
}

// A book's details, in a file of `scratch` to send; empty when it can't be written.
[[nodiscard]] QString writeDetails(const QTemporaryDir &scratch, int &counter, const QJsonDocument &document) {
    const QString file = scratch.filePath(QString("details-%1.json").arg(++counter));
    QFile json(file);
    return json.open(QIODevice::WriteOnly) && json.write(document.toJson()) > 0 ? file : QString();
}

// The two sides of a sync, for the guard to plan, check and run it (#187):
// this sync once planned, and the other side's books as it says them.
[[nodiscard]] SyncGuard::Sides guardedSides(Library &library, const Remote &remote, const std::function<void(const QString &)> &progress) {
    return {.host = remote.host(),
            .hub = remote.hub(),
            .run =
                [&library, &remote, progress](SyncOptions options) {
                    options.planned = true;
                    return syncLibrary(library, remote, progress, options);
                },
            .theirStates = [&remote] -> std::optional<SyncPlan::States> {
                const Remote::Result said = remote.omabook({"manifest", "--state"});
                const QJsonDocument states = QJsonDocument::fromJson(said.output);
                if (!said.ok() || !states.isObject())
                    return std::nullopt;
                return SyncPlan::statesFrom(states.object());
            }};
}

// Whether the other side has this file: by its hash, or by the one it came in with (#74).
[[nodiscard]] bool knownThere(const FormatFile &file, const QSet<QString> &theirHashes) {
    return theirHashes.contains(file.hash) || (!file.original.isEmpty() && theirHashes.contains(file.original));
}

} // namespace

SyncSummary syncLibrary(Library &library, const Remote &remote, const std::function<void(const QString &)> &progress,
                        const SyncOptions &options) {
    if (!options.planned)
        return SyncGuard::guarded(library, guardedSides(library, remote, progress), options);
    SyncSummary summary;
    summary.dryRun = options.dryRun;
    const bool dryRun = options.dryRun;
    std::optional<QJsonObject> listed = theirManifest(library, remote, summary);
    if (!listed)
        return summary;
    QJsonObject manifest = std::move(*listed);
    // An Omabook there that keeps when details changed and when books were
    // removed, which can take details and be asked to remove a book. With
    // an older one the books and reading travel as they always did.
    const bool peerHasLog = manifest.value("log").toInt() >= 1;
    const bool current = manifest.contains("removals") || peerHasLog;
    if (!dryRun)
        SyncManifest::observeManifest(library, manifest);
    // Between two that speak the log, it goes first: each side takes what is
    // newer on the other, and the books this one lacks arrive with what it says.
    QSet<qint64> gone; // the books here they removed, which the log took care of
    // What plugins' folders hold, recorded on each side before the log goes, and carried once it went.
    const std::optional<PluginFileSync::Sides> pluginFiles =
        peerHasLog && !dryRun ? PluginFileSync::begin(library, remote, options.plugins) : std::nullopt;
    if (peerHasLog) {
        SyncLog::Replicated replicated = SyncLog::replicate(library, remote, manifest, summary, options);
        // Their books as the log says them, the vector both now hold, and who they are now.
        manifest = std::move(replicated.manifest);
        gone = std::move(replicated.gone);
    }
    QTemporaryDir scratch;
    int counter = 0;
    QSet<QString> theirHashes;
    // Books without a file are known by their entry keys: every book here by its keys, and every key there.
    QHash<QString, qint64> myKeys = keysHere(library);
    QSet<QString> theirKeys;

    // One folder there for the details and covers sent, made when first needed.
    QString folderThere;
    const auto dropFolder = qScopeGuard([&] {
        if (!folderThere.isEmpty())
            remote.dropFolder(folderThere);
    });
    const OlderPeer::Sync olderPeer{library, remote, scratch, folderThere, counter, options, summary, current};

    // What they have: fetch the books this library lacks, and for the ones
    // both have, the formats it lacks, the newer details and reading place.
    for (const QJsonValue &value : manifest.value("books").toArray()) {
        const QJsonObject theirs = value.toObject();
        const QString title = theirs.value("title").toString();
        const QJsonArray files = theirs.value("files").toArray();
        auto [mine, removedHere, hashes] = namedHere(library, files, theirHashes);
        const QStringList keys = entryKeysOf(theirs);
        for (const QString &key : keys)
            theirKeys << key;
        // A book without a file there is the one here of the same name.
        if (files.isEmpty()) {
            for (const QString &key : keys) {
                mine = mine ? mine : myKeys.value(key);
                removedHere = removedHere || library.wasRemoved(key);
            }
        }
        // Removed here, so never fetched back. Between two that speak the log
        // the other side removed it too, as the log said, unless it changed
        // there since; an older one is asked to.
        if (!mine && removedHere) {
            if (!peerHasLog)
                OlderPeer::removeThere(olderPeer, theirs, hashes, keys);
            continue;
        }
        if (!mine && dryRun) {
            note(summary.received, summary.titles.received, title);
            continue;
        }
        if (progress && !mine)
            progress(title);

        auto fetched = [&](const QJsonObject &file) -> QString {
            const QString local = scratch.filePath(QString("%1.%2").arg(++counter).arg(file.value("format").toString()));
            if (!remote.fetch(file.value("path").toString(), local)
                || Library::hashFile(local) != file.value("hash").toString()) {
                summary.problems << title + " did not arrive whole from " + remote.host() + ".";
                QFile::remove(local);
                return {};
            }
            return local;
        };
        // What arrives keeps the time its details had there.
        const qint64 theirDetailsAt = SyncManifest::theirDetailsHlc(theirs);

        if (!mine && files.isEmpty()) {
            BookMeta meta = BookMeta::fromJson(theirs);
            const QString cover = theirs.value("cover").toString();
            if (!cover.isEmpty() && remote.fetch(cover, scratch.filePath("cover.jpg")))
                meta.cover = QImageReader(scratch.filePath("cover.jpg")).read();
            const Library::Added added = addTheirs(library, meta, theirs);
            if (!added.error.isEmpty()) {
                summary.problems << added.error;
                continue;
            }
            SyncTake::fillReading(library, added.id, theirs, false);
            if (current)
                library.setDetailsAt(added.id, theirDetailsAt);
            if (const std::optional<Book> made = library.book(added.id))
                for (const QString &key : Library::entryKeys(*made))
                    myKeys.insert(key, added.id);
            note(summary.received, summary.titles.received, title);
            continue;
        }
        const bool arrived = !mine;
        if (arrived) {
            const QString first = fetched(files.first().toObject());
            if (first.isEmpty())
                continue;
            BookMeta meta = BookMeta::fromJson(theirs);
            const QString cover = theirs.value("cover").toString();
            if (!cover.isEmpty() && remote.fetch(cover, scratch.filePath("cover.jpg")))
                meta.cover = QImageReader(scratch.filePath("cover.jpg")).read();
            const Library::Added added = library.add(first, meta);
            QFile::remove(first);
            if (!added.error.isEmpty()) {
                summary.problems << added.error;
                continue;
            }
            mine = added.id;
            library.knowOriginal(mine, files.first().toObject().value("format").toString(),
                                 files.first().toObject().value("original").toString());
            if (current)
                library.setDetailsAt(mine, theirDetailsAt);
            note(summary.received, summary.titles.received, title);
        }

        if (!peerHasLog)
            OlderPeer::mergeBook(olderPeer, mine, arrived, theirs);
        else if (arrived)
            SyncTake::fillReading(library, mine, theirs, false);
        if (files.isEmpty())
            continue;
        const QStringList have = library.book(mine)->formats;
        for (const QJsonValue &file : files) {
            if (dryRun || have.contains(file.toObject().value("format").toString()))
                continue;
            const QString local = fetched(file.toObject());
            QString error;
            if (!local.isEmpty() && !library.addFormat(mine, local, &error))
                summary.problems << error;
            QFile::remove(local);
        }
    }

    // What they lack: send it, unless they removed it themselves; and when
    // they removed all of it and it didn't change here since, remove it here
    // (between two that speak the log, as it says, already).
    const QSet<QString> removedThere = peerHasLog ? QSet<QString>() : OlderPeer::removedThere(manifest);
    for (const Book &book : library.books()) {
        if (gone.contains(book.id))
            continue;
        const QList<FormatFile> files = library.formatFiles(book.id);
        bool known = false, removed = false;
        QStringList hashes, keys;
        for (const FormatFile &file : files) {
            hashes << file.hash;
            known = known || knownThere(file, theirHashes);
            removed = removed || removedThere.contains(file.hash);
        }
        if (files.isEmpty())
            for (const QString &key : Library::entryKeys(book)) {
                keys << key;
                known = known || theirKeys.contains(key);
                removed = removed || removedThere.contains(key);
            }
        if (known)
            continue;
        if (removed) {
            OlderPeer::removeHere(olderPeer, book, hashes, keys, manifest);
            continue;
        }
        if (dryRun) {
            note(summary.sent, summary.titles.sent, book.meta.title);
            continue;
        }
        if (progress)
            progress(book.meta.title);

        const QString folder = remote.makeFolder();
        if (folder.isEmpty()) {
            summary.problems << "No room to receive books on " + remote.host() + ".";
            break;
        }
        // Whatever happens next, the other computer's scratch folder goes.
        const auto cleanup = qScopeGuard([&] { remote.dropFolder(folder); });

        QStringList sentFiles;
        QString why;
        for (const FormatFile &file : files) {
            const QString target = folder + "/book." + file.format;
            if (why.isEmpty())
                if (const auto sent = remote.send(file.path, target); !sent)
                    why = file.format.toUpper() + ": " + sent.error();
            sentFiles << target;
        }
        const QString details = why.isEmpty() ? writeDetails(scratch, counter, QJsonDocument(book.meta.toJson())) : QString();
        if (why.isEmpty() && details.isEmpty())
            why = "its details can't be written here";
        if (why.isEmpty())
            if (const auto sent = remote.send(details, folder + "/details.json"); !sent)
                why = sent.error();
        // A book without a file is added as one, then given its reading.
        QStringList add = files.isEmpty() ? QStringList{"add", book.paper ? "--paper" : "--wish"} : QStringList{"add", sentFiles.value(0)};
        add << "--meta" << folder + "/details.json" << "--json";
        if (const QString cover = library.coverPath(book.id); !cover.isEmpty() && remote.send(cover, folder + "/cover.jpg"))
            add << "--cover" << folder + "/cover.jpg";
        const Remote::Result added = why.isEmpty() ? remote.omabook(add) : Remote::Result();
        // `add` of files answers with a list, `add --paper` with the book.
        const QJsonDocument answer = QJsonDocument::fromJson(added.output);
        const qint64 theirId = (answer.isArray() ? answer.array().first().toObject() : answer.object()).value("id").toInteger();
        if (why.isEmpty() && !added.ok())
            why = added.error.section('\n', -1);
        else if (why.isEmpty() && theirId <= 0)
            why = "Omabook there added nothing: " + QString::fromUtf8(added.output).simplified().left(160);
        if (!why.isEmpty()) {
            // A book in a format of a plugin there isn't, which that Omabook doesn't keep for it either: say which.
            if (why.contains("is not a book format Omabook knows"))
                why = notReadThere(files.isEmpty() ? QString() : files.first().format);
            summary.problems << book.meta.title + " could not be sent to " + remote.host() + ": " + why;
            qCWarning(lcSync).noquote() << "a book could not be sent:" << why.left(200);
            continue;
        }
        for (const QString &file : sentFiles.mid(1))
            remote.omabook({"add", file, "--to", QString::number(theirId)});
        if (!peerHasLog)
            OlderPeer::givePlace(library, remote, book.id, theirId);
        // Its details there keep the time they have here.
        const qint64 myBookDetailsAt =
            book.detailsAt == 0 ? 0 : SyncManifest::wireTimestamp(SyncManifest::myDetailsHlc(library, book), peerHasLog);
        remote.omabook(SyncTake::readingArguments(theirId, book, current ? std::optional<qint64>(myBookDetailsAt) : std::nullopt));
        note(summary.sent, summary.titles.sent, book.meta.title);
    }
    // The collections and the knowledge graph, once both sides have every book: theirs taken in
    // here (a collection where it changed last, the graph merged), and this library's handed to
    // them to do the same. An Omabook from before them has none to give and can't take any, and
    // a dry run leaves them be.
    if (!dryRun)
        syncCollectionsAndGraph(library, remote, manifest, summary);
    // Books that moved arrived after the log went: once more for what their
    // arrival wrote, and their reading places, and the spaces one is in.
    if (peerHasLog && !dryRun)
        SyncLog::finish(library, remote, manifest, summary, options);
    if (pluginFiles)
        PluginFileSync::finish(library, remote, options.plugins, *pluginFiles, summary);
    logSyncSummary(summary, remote.host());
    return summary;
}
