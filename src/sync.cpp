#include "sync.h"

#include "log.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <optional>

#include "collections.h"
#include "formats.h"
#include "graphsync.h"
#include "hlc.h"
#include "olderpeer.h"
#include "replication.h"
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

} // namespace

SyncSummary syncLibrary(Library &library, const Remote &remote, const std::function<void(const QString &)> &progress,
                        const SyncOptions &options) {
    SyncSummary summary;
    summary.dryRun = options.dryRun;
    const bool dryRun = options.dryRun;
    // An Omabook that speaks the log answers `manifest --log` with a manifest
    // that says so and leaves out what the log carries; one from before
    // refuses the option it doesn't know (exit code 1), and is asked for the
    // manifest it has always given.
    Remote::Result listed = remote.omabook({"manifest", "--log"});
    if (listed.code == 1)
        listed = remote.omabook({"manifest"});
    QJsonObject manifest = QJsonDocument::fromJson(listed.output).object();
    if (!listed.ok() || !manifest.contains("books")) {
        summary.error = listed.code == 127 || listed.error.contains("not found")
                            ? "Omabook isn't installed on " + remote.host() + ", or isn't on its PATH."
                            : listed.error.isEmpty() ? remote.host() + " can't be reached over ssh."
                                                     : remote.host() + ": " + listed.error.section('\n', -1);
        return summary;
    }
    // A manifest in a newer shape than this Omabook knows could be misread,
    // and a sync acts on what it reads; better to say which side to update.
    if (manifest.value("version").toInt(1) > manifestVersion) {
        summary.error = QString("%1 runs a newer Omabook (%2) than this computer (%3), and the two can't sync until this one "
                                "is updated.")
                            .arg(remote.host(), manifest.value("omabook").toString("unknown"), QStringLiteral(OMABOOK_VERSION));
        return summary;
    }
    // An Omabook there that keeps when details changed and when books were
    // removed, which can take details and be asked to remove a book. With
    // an older one the books and reading travel as they always did.
    const bool current = manifest.contains("removals");
    const bool peerHasLog = manifest.value("log").toInt() >= 1;
    if (!dryRun)
        SyncManifest::observeManifest(library, manifest);
    // Between two that speak the log, it goes first: each side takes what is
    // newer on the other, and the books this one lacks arrive with what it says.
    if (peerHasLog) {
        manifest.insert("books", SyncLog::replicate(library, remote, manifest, summary, options));
        // Both now hold the same operations: what this side holds, they do.
        manifest.insert("vector", Replication::toJson(Replication::vector(library)));
    }
    QTemporaryDir scratch;
    int counter = 0;
    QSet<QString> theirHashes;
    // Books without a file are known by their entry keys (Library::entryKeys):
    // every book here by its keys, and every key there.
    QHash<QString, qint64> myKeys;
    for (const Book &book : library.books())
        for (const QString &key : Library::entryKeys(book))
            myKeys.insert(key, book.id);
    QSet<QString> theirKeys;

    // One folder there for the details and covers sent, made when first needed.
    QString folderThere;
    const auto dropFolder = qScopeGuard([&] {
        if (!folderThere.isEmpty())
            remote.run("rm -rf " + Remote::quote(folderThere));
    });
    const auto writeJson = [&](const QJsonDocument &document) -> QString {
        const QString file = scratch.filePath(QString("details-%1.json").arg(++counter));
        QFile json(file);
        return json.open(QIODevice::WriteOnly) && json.write(document.toJson()) > 0 ? file : QString();
    };
    const OlderPeer::Sync olderPeer{library, remote, scratch, folderThere, counter, options, summary, current};

    // What they have: fetch the books this library lacks, and for the ones
    // both have, the formats it lacks, the newer details and reading place.
    for (const QJsonValue &value : manifest.value("books").toArray()) {
        const QJsonObject theirs = value.toObject();
        const qint64 theirId = theirs.value("id").toInteger();
        const QString title = theirs.value("title").toString();
        const QJsonArray files = theirs.value("files").toArray();
        qint64 mine = 0;
        bool removedHere = false;
        QStringList hashes;
        for (const QJsonValue &file : files) {
            const QString hash = file.toObject().value("hash").toString();
            hashes << hash;
            theirHashes << hash;
            if (!mine)
                mine = library.bookWithHash(hash);
            removedHere = removedHere || library.wasRemoved(hash);
        }
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
        // Removed here, so never fetched back; and removed there too, when
        // all of it was removed here and it didn't change there since. One
        // with a file this library still holds is a book here (`mine`), as
        // the one a merge kept is, and is never removed.
        if (!mine && removedHere) {
            const std::optional<qint64> when =
                SyncManifest::removedWhen(hashes, keys, [&](const QString &key) { return SyncManifest::localRemovalHlc(library, key); });
            if (current && when && std::is_lteq(Hlc::compare(SyncManifest::lastChangeHlc(theirs), *when))) {
                const Remote::Result removed =
                    dryRun ? Remote::Result{.code = 0, .output = {}, .error = {}} : remote.omabook({"remove", QString::number(theirId)});
                if (removed.ok()) {
                    note(summary.removedThere, summary.titles.removedThere, title);
                    if (!dryRun)
                        qCInfo(lcSync).noquote() << "book" << theirId << "removed on" << remote.host() << "as it was here";
                } else {
                    summary.problems << title + " could not be removed on " + remote.host() + ": " + removed.error.section('\n', -1);
                }
            }
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
            const Library::Added added = library.addEntry(meta, theirs.value("paper").toBool());
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
    // they removed all of it and it didn't change here since, remove it here.
    QSet<QString> removedThere;
    for (const QJsonValue &hash : manifest.value("removed").toArray())
        removedThere << hash.toString();
    const QHash<QString, qint64> removalsThere = SyncManifest::remoteRemovalHlcs(manifest);
    for (auto it = removalsThere.cbegin(); it != removalsThere.cend(); ++it)
        removedThere << it.key();
    for (const Book &book : library.books()) {
        const QList<FormatFile> files = library.formatFiles(book.id);
        bool known = false, removed = false;
        QStringList hashes, keys;
        for (const FormatFile &file : files) {
            hashes << file.hash;
            known = known || theirHashes.contains(file.hash);
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
            const std::optional<qint64> when = SyncManifest::removedWhen(hashes, keys, [&](const QString &key) {
                return removalsThere.contains(key) ? std::optional<qint64>(removalsThere.value(key)) : std::nullopt;
            });
            if (current && when && std::is_lteq(Hlc::compare(SyncManifest::lastChangeHlc(library, book), *when))) {
                QString error;
                if (dryRun || library.remove(book.id, &error)) {
                    note(summary.removedHere, summary.titles.removedHere, book.meta.title);
                    if (!dryRun)
                        qCInfo(lcSync).noquote() << "book" << book.id << "removed here, as it was on" << remote.host();
                } else {
                    summary.problems << book.meta.title + " could not be removed here: " + error;
                }
            }
            continue;
        }
        if (dryRun) {
            note(summary.sent, summary.titles.sent, book.meta.title);
            continue;
        }
        if (progress)
            progress(book.meta.title);

        const QString folder = QString::fromUtf8(remote.run("mktemp -d").output).trimmed();
        if (folder.isEmpty()) {
            summary.problems << "No room to receive books on " + remote.host() + ".";
            break;
        }
        // Whatever happens next, the other computer's scratch folder goes.
        const auto cleanup = qScopeGuard([&] { remote.run("rm -rf " + Remote::quote(folder)); });

        QStringList sentFiles;
        QString why;
        for (const FormatFile &file : files) {
            const QString target = folder + "/book." + file.format;
            if (why.isEmpty())
                if (const auto sent = remote.send(file.path, target); !sent)
                    why = file.format.toUpper() + ": " + sent.error();
            sentFiles << target;
        }
        const QString details = why.isEmpty() ? writeJson(QJsonDocument(book.meta.toJson())) : QString();
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
            // A book in a format of a plugin there isn't: say which.
            if (why.contains("is not a book format Omabook knows"))
                why = QString("there, nothing reads .%1 files; install the plugin that made it there (for a story: omabook plugin add "
                              "storyteller)").arg(files.isEmpty() ? QString() : files.first().format);
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
    // arrival wrote, and their reading places.
    if (peerHasLog && !dryRun && summary.received + summary.sent > 0)
        SyncLog::replicate(library, remote, manifest, summary, options);
    logSyncSummary(summary, remote.host());
    return summary;
}
