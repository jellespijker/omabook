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

namespace {

Progress progressFrom(const QJsonObject &object) {
    Progress progress;
    progress.section = object.value("section").toInt();
    progress.fraction = object.value("fraction").toDouble();
    progress.percent = object.value("percent").toDouble();
    progress.updated = object.value("updated").toInteger();
    return progress;
}

QStringList progressArguments(qint64 id, const Progress &progress) {
    return {"progress", QString::number(id), "--section", QString::number(progress.section), "--fraction",
            QString::number(progress.fraction, 'g', 12), "--percent", QString::number(progress.percent, 'g', 12),
            "--updated", QString::number(progress.updated)};
}

// What names a book of the other side's manifest that has no file.
QStringList entryKeysOf(const QJsonObject &theirs) {
    return Library::entryKeys(theirs.value("identifiers").toObject(), theirs.value("isbn").toString(), theirs.value("title").toString(),
                              BookMeta::fromJson(theirs).authors);
}

// `omabook set` arguments that give the other side this book's reading and
// its names elsewhere: the shelf, stars, finished day and paper copy.
// With `detailsAt` (for an Omabook there that keeps it), the time its
// details are stamped with there: the names elsewhere are part of the
// details, and what a sync brings keeps the time it had.
//
// "generated" (which plugin made a book) never travels: only that plugin
// marks a book as its own, and the mark is what lets it fork and branch a
// story, which must never reach a book by a human author. A sync neither
// sends it nor takes it in.
bool travels(const QString &scheme) { return scheme.compare(QStringLiteral("generated"), Qt::CaseInsensitive) != 0; }

QStringList readingArguments(qint64 id, const Book &book, std::optional<qint64> detailsAt) {
    QStringList arguments{"set", QString::number(id), "--status", book.shelf.isEmpty() ? QStringLiteral("none") : book.shelf,
                          "--rating", QString::number(book.rating), "--paper", book.paper ? QStringLiteral("yes") : QStringLiteral("no")};
    if (!book.finished.isEmpty())
        arguments << "--finished" << book.finished;
    for (auto it = book.identifiers.begin(); it != book.identifiers.end(); ++it)
        if (const QString value = it.value().toVariant().toString(); !value.isEmpty() && travels(it.key()))
            arguments << "--identifier" << it.key() + "=" + value;
    if (detailsAt)
        arguments << "--details-at" << QString::number(*detailsAt);
    return arguments;
}

// Takes in what the other side says of a book's reading where this side
// says nothing: an empty shelf, no stars, no finished day; a paper copy
// either side has is had. Whether that changes anything; with `dryRun`
// only that.
bool fillReading(Library &library, qint64 id, const QJsonObject &theirs, bool dryRun) {
    const std::optional<Book> mine = library.book(id);
    if (!mine)
        return false;
    bool changed = false;
    const QString shelf = mine->shelf.isEmpty() ? theirs.value("shelf").toString() : mine->shelf;
    const int rating = mine->rating > 0 ? mine->rating : theirs.value("rating").toInt();
    const QString finished = mine->finished.isEmpty() ? theirs.value("finished").toString() : mine->finished;
    if (shelf != mine->shelf || rating != mine->rating || finished != mine->finished) {
        changed = true;
        if (!dryRun)
            library.setReading(id, shelf, rating, finished);
    }
    if (!mine->paper && theirs.value("paper").toBool()) {
        changed = true;
        if (!dryRun)
            library.setPaper(id, true);
    }
    // Names that came by sync keep the details' time, so they are no
    // reason to send the details back.
    const QJsonObject identifiers = theirs.value("identifiers").toObject();
    for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
        if (travels(it.key()) && !mine->identifiers.contains(it.key())) {
            changed = true;
            if (!dryRun)
                library.setIdentifier(id, it.key(), it.value().toVariant().toString(), mine->detailsAt);
        }
    return changed;
}

// Whether the other side says less of a book's reading than this one does
// now, so it is given this side's.
bool saysLess(const QJsonObject &theirs, const Book &mine) {
    if ((theirs.value("shelf").toString().isEmpty() && !mine.shelf.isEmpty()) || (theirs.value("rating").toInt() == 0 && mine.rating > 0)
        || (theirs.value("finished").toString().isEmpty() && !mine.finished.isEmpty()) || (!theirs.value("paper").toBool() && mine.paper))
        return true;
    const QJsonObject identifiers = theirs.value("identifiers").toObject();
    return std::ranges::any_of(mine.identifiers.keys(), [&](const QString &scheme) { return travels(scheme) && !identifiers.contains(scheme); });
}

// A book's details as both sides keep them, to compare.
QJsonObject detailsOf(const BookMeta &meta) { return BookMeta::fromJson(meta.toJson()).toJson(); }

// The last time a book changed in a way a removal elsewhere must not
// overlook: when it was added, read, its details or its reading.
qint64 lastChange(const Book &book) { return std::max({book.added, book.lastRead, book.detailsAt, book.readingAt}); }

qint64 lastChange(const QJsonObject &theirs) {
    return std::max({theirs.value("added_at").toInteger(), theirs.value("progress").toObject().value("updated").toInteger(),
                     theirs.value("details_at").toInteger(), theirs.value("reading_at").toInteger()});
}

// When a book was removed on a side. One with files was when every one of
// them was removed there, and it is the earliest of those times, so a change
// after any of them keeps the book; one without a file was when one of its
// names was, the earliest of those. None when it wasn't.
std::optional<qint64> removedWhen(const QStringList &hashes, const QStringList &keys,
                                  const std::function<std::optional<qint64>(const QString &)> &at) {
    std::optional<qint64> earliest;
    for (const QString &key : hashes.isEmpty() ? keys : hashes) {
        const std::optional<qint64> when = at(key);
        if (!when && !hashes.isEmpty())
            return std::nullopt;
        if (when)
            earliest = std::min(earliest.value_or(*when), *when);
    }
    return earliest;
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

QJsonObject SyncSummary::toJson() const {
    const auto list = [](const QStringList &titles) { return QJsonArray::fromStringList(titles); };
    QJsonObject result{{"received", received}, {"sent", sent}, {"places_taken", placesTaken}, {"places_sent", placesSent},
                       {"details_taken", detailsTaken}, {"details_sent", detailsSent}, {"reading_taken", readingTaken},
                       {"reading_sent", readingSent}, {"removed_here", removedHere}, {"removed_there", removedThere},
                       {"details_undecided", detailsUndecided}, {"collections_taken", collectionsTaken},
                       {"collections_sent", collectionsSent}, {"graph_taken", graphTaken}, {"graph_sent", graphSent},
                       {"dry_run", dryRun}, {"problems", list(problems)}};
    QJsonObject named{{"details_undecided", list(titles.detailsUndecided)}};
    if (dryRun)
        named = QJsonObject{{"received", list(titles.received)}, {"sent", list(titles.sent)},
                            {"places_taken", list(titles.placesTaken)}, {"places_sent", list(titles.placesSent)},
                            {"details_taken", list(titles.detailsTaken)}, {"details_sent", list(titles.detailsSent)},
                            {"reading_taken", list(titles.readingTaken)}, {"reading_sent", list(titles.readingSent)},
                            {"removed_here", list(titles.removedHere)}, {"removed_there", list(titles.removedThere)},
                            {"details_undecided", list(titles.detailsUndecided)}};
    result.insert("titles", named);
    return result;
}

SyncSummary syncLibrary(Library &library, const Remote &remote, const std::function<void(const QString &)> &progress,
                        const SyncOptions &options) {
    SyncSummary summary;
    summary.dryRun = options.dryRun;
    const bool dryRun = options.dryRun;
    const Remote::Result listed = remote.omabook({"manifest"});
    const QJsonObject manifest = QJsonDocument::fromJson(listed.output).object();
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
    const auto scratchThere = [&]() -> QString {
        if (folderThere.isEmpty())
            folderThere = QString::fromUtf8(remote.run("mktemp -d").output).trimmed();
        return folderThere;
    };
    const auto writeJson = [&](const QJsonDocument &document) -> QString {
        const QString file = scratch.filePath(QString("details-%1.json").arg(++counter));
        QFile json(file);
        return json.open(QIODevice::WriteOnly) && json.write(document.toJson()) > 0 ? file : QString();
    };

    // What they have: fetch the books this library lacks, and for the ones
    // both have, the formats it lacks, the newer details and reading place.
    for (const QJsonValue &value : manifest.value("books").toArray()) {
        const QJsonObject theirs = value.toObject();
        const qint64 theirId = theirs.value("id").toInteger();
        const QString title = theirs.value("title").toString();
        const QJsonArray files = theirs.value("files").toArray();
        const Progress theirPlace = progressFrom(theirs.value("progress").toObject());
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
            const std::optional<qint64> when = removedWhen(hashes, keys, [&](const QString &key) { return library.removedAt(key); });
            if (current && when && lastChange(theirs) <= *when) {
                const Remote::Result removed = dryRun ? Remote::Result{.code = 0, .output = {}, .error = {}}
                                                      : remote.omabook({"remove", QString::number(theirId)});
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
        const qint64 theirDetailsAt = theirs.value("details_at").toInteger();

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
            fillReading(library, added.id, theirs, false);
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

        // The details: the newer side's win, with the cover when it is
        // another picture. Taken or given, they keep the time they had, so
        // the next sync finds both alike and does nothing. Where no time
        // says which are newer (both from before times were kept) only
        // the words are compared, and they are left as they are unless the
        // user chose a side.
        std::optional<qint64> detailsThere = current ? std::optional<qint64>(theirDetailsAt) : std::nullopt;
        if (!arrived) {
            const Book here = *library.book(mine);
            const qint64 myAt = here.detailsAt;
            const bool wordsDiffer = detailsOf(BookMeta::fromJson(theirs)) != detailsOf(here.meta);
            const QString theirCover = theirs.value("cover_hash").toString();
            const QString myCoverFile = library.coverPath(mine);
            const QString myCover = myCoverFile.isEmpty() ? QString() : Library::hashFile(myCoverFile);
            const bool coverDiffers = current && theirCover != myCover;
            enum class Way { None, Take, Send, Undecided } way = Way::None;
            if (theirDetailsAt != myAt && (wordsDiffer || coverDiffers))
                way = theirDetailsAt > myAt ? Way::Take : Way::Send;
            else if (theirDetailsAt == myAt && wordsDiffer)
                way = options.details == SyncOptions::Details::Mine     ? Way::Send
                      : options.details == SyncOptions::Details::Theirs ? Way::Take
                                                                        : Way::Undecided;
            // An Omabook from before can't be given details.
            if (way == Way::Send && !current)
                way = Way::None;
            if (way == Way::Undecided) {
                note(summary.detailsUndecided, summary.titles.detailsUndecided, title);
            } else if (way == Way::Take) {
                BookMeta meta = BookMeta::fromJson(theirs);
                const QString cover = theirs.value("cover").toString();
                const QString picture = scratch.filePath(QString("cover-%1.jpg").arg(++counter));
                if (!dryRun && coverDiffers && !theirCover.isEmpty() && !cover.isEmpty() && remote.fetch(cover, picture))
                    meta.cover = QImageReader(picture).read();
                QString error;
                if (dryRun || library.setMeta(mine, meta, &error, theirDetailsAt))
                    note(summary.detailsTaken, summary.titles.detailsTaken, title);
                else
                    summary.problems << title + ": " + error;
            } else if (way == Way::Send) {
                bool given = dryRun;
                if (!dryRun) {
                    const QString folder = scratchThere();
                    const QString details = writeJson(QJsonDocument(here.meta.toJson()));
                    QStringList set{"set", QString::number(theirId), "--meta", folder + "/details.json", "--details-at",
                                    QString::number(myAt)};
                    if (coverDiffers && !myCoverFile.isEmpty() && remote.send(myCoverFile, folder + "/cover.jpg"))
                        set << "--cover" << folder + "/cover.jpg";
                    given = !folder.isEmpty() && !details.isEmpty() && remote.send(details, folder + "/details.json")
                            && remote.omabook(set).ok();
                }
                if (given) {
                    note(summary.detailsSent, summary.titles.detailsSent, title);
                    detailsThere = myAt;
                } else {
                    summary.problems << title + ": its details could not be given to " + remote.host() + ".";
                }
            }
        }

        if (fillReading(library, mine, theirs, dryRun) && !arrived)
            note(summary.readingTaken, summary.titles.readingTaken, title);
        // Older Omabooks don't say "shelf"; they are given nothing they can't keep.
        if (const std::optional<Book> merged = library.book(mine); merged && theirs.contains("shelf") && saysLess(theirs, *merged)) {
            if (dryRun || remote.omabook(readingArguments(theirId, *merged, detailsThere)).ok())
                note(summary.readingSent, summary.titles.readingSent, title);
        }
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

        const Progress myPlace = library.progress(mine);
        if (theirPlace.updated > myPlace.updated) {
            if (!dryRun)
                library.setProgress(mine, theirPlace);
            note(summary.placesTaken, summary.titles.placesTaken, title);
        } else if (myPlace.updated > theirPlace.updated) {
            if (dryRun || remote.omabook(progressArguments(theirId, myPlace)).ok())
                note(summary.placesSent, summary.titles.placesSent, title);
        }
    }

    // What they lack: send it, unless they removed it themselves; and when
    // they removed all of it and it didn't change here since, remove it here.
    QSet<QString> removedThere;
    for (const QJsonValue &hash : manifest.value("removed").toArray())
        removedThere << hash.toString();
    QHash<QString, qint64> removalsThere;
    for (const QJsonValue &removal : manifest.value("removals").toArray()) {
        const QString key = removal.toObject().value("key").toString();
        removalsThere.insert(key, removal.toObject().value("at").toInteger());
        removedThere << key;
    }
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
            const std::optional<qint64> when = removedWhen(hashes, keys, [&](const QString &key) {
                return removalsThere.contains(key) ? std::optional<qint64>(removalsThere.value(key)) : std::nullopt;
            });
            if (current && when && lastChange(book) <= *when) {
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
        if (const Progress place = library.progress(book.id); place.updated > 0)
            remote.omabook(progressArguments(theirId, place));
        // Its details there keep the time they have here.
        remote.omabook(readingArguments(theirId, book, current ? std::optional<qint64>(book.detailsAt) : std::nullopt));
        note(summary.sent, summary.titles.sent, book.meta.title);
    }
    // The collections and the knowledge graph, once both sides have every book: theirs taken in
    // here (a collection where it changed last, the graph merged), and this library's handed to
    // them to do the same. An Omabook from before them has none to give and can't take any, and
    // a dry run leaves them be.
    if (!dryRun)
        syncCollectionsAndGraph(library, remote, manifest, summary);
    logSyncSummary(summary, remote.host());
    return summary;
}
