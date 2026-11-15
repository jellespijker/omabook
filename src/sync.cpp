#include "sync.h"

#include "log.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>

#include "collections.h"
#include "formats.h"

namespace {

Progress progressFrom(const QJsonObject &object) {
    Progress progress;
    progress.section = object.value("section").toInt();
    progress.fraction = object.value("fraction").toDouble();
    progress.percent = object.value("percent").toDouble();
    progress.updated = object.value("updated").toInteger();
    return progress;
}

QJsonObject toJson(const Progress &progress) {
    return {{"section", progress.section}, {"fraction", progress.fraction}, {"percent", progress.percent},
            {"updated", progress.updated}};
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
QStringList readingArguments(qint64 id, const Book &book) {
    QStringList arguments{"set", QString::number(id), "--status", book.shelf.isEmpty() ? QStringLiteral("none") : book.shelf,
                          "--rating", QString::number(book.rating), "--paper", book.paper ? QStringLiteral("yes") : QStringLiteral("no")};
    if (!book.finished.isEmpty())
        arguments << "--finished" << book.finished;
    for (auto it = book.identifiers.begin(); it != book.identifiers.end(); ++it)
        if (const QString value = it.value().toVariant().toString(); !value.isEmpty())
            arguments << "--identifier" << it.key() + "=" + value;
    return arguments;
}

// Takes in what the other side says of a book's reading where this side
// says nothing: an empty shelf, no stars, no finished day; a paper copy
// either side has is had.
void fillReading(Library &library, qint64 id, const QJsonObject &theirs) {
    const std::optional<Book> mine = library.book(id);
    if (!mine)
        return;
    const QString shelf = mine->shelf.isEmpty() ? theirs.value("shelf").toString() : mine->shelf;
    const int rating = mine->rating > 0 ? mine->rating : theirs.value("rating").toInt();
    const QString finished = mine->finished.isEmpty() ? theirs.value("finished").toString() : mine->finished;
    if (shelf != mine->shelf || rating != mine->rating || finished != mine->finished)
        library.setReading(id, shelf, rating, finished);
    if (!mine->paper && theirs.value("paper").toBool())
        library.setPaper(id, true);
    const QJsonObject identifiers = theirs.value("identifiers").toObject();
    for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
        if (!mine->identifiers.contains(it.key()))
            library.setIdentifier(id, it.key(), it.value().toVariant().toString());
}

// Whether the other side says less of a book's reading than this one does
// now, so it is given this side's.
bool saysLess(const QJsonObject &theirs, const Book &mine) {
    if ((theirs.value("shelf").toString().isEmpty() && !mine.shelf.isEmpty()) || (theirs.value("rating").toInt() == 0 && mine.rating > 0)
        || (theirs.value("finished").toString().isEmpty() && !mine.finished.isEmpty()) || (!theirs.value("paper").toBool() && mine.paper))
        return true;
    const QJsonObject identifiers = theirs.value("identifiers").toObject();
    return std::ranges::any_of(mine.identifiers.keys(), [&](const QString &scheme) { return !identifiers.contains(scheme); });
}

} // namespace

Remote::Remote(const QString &target, const QStringList &ssh)
    : m_host(target.section(':', 0, 0).trimmed()),
      m_folder(target.contains(':') ? target.section(':', 1).trimmed() : QString()),
      // Never a password prompt nobody can see, and no wait on a computer that is off.
      m_ssh(ssh.isEmpty() ? QStringList{"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"} : ssh) {}

QString Remote::quote(const QString &argument) { return "'" + QString(argument).replace("'", "'\\''") + "'"; }

Remote::Result Remote::run(const QString &command, const QString &inputFile, const QString &outputFile) const {
    Result result;
    QProcess process;
    if (!inputFile.isEmpty())
        process.setStandardInputFile(inputFile);
    if (!outputFile.isEmpty())
        process.setStandardOutputFile(outputFile);
    // "--" ends ssh's own options, so a host can never be read as one.
    process.start(m_ssh.first(), m_ssh.mid(1) + QStringList{"--", m_host, command});
    if (!process.waitForStarted(5000)) {
        result.error = "ssh can't be started.";
        return result;
    }
    if (inputFile.isEmpty())
        process.closeWriteChannel();
    // A large book over a slow link takes a while; an hour is a hung link.
    if (!process.waitForFinished(3600 * 1000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = "The connection to " + m_host + " stopped answering.";
        return result;
    }
    result.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.output = process.readAllStandardOutput();
    result.error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return result;
}

Remote::Result Remote::omabook(const QStringList &arguments) const {
    // An ssh command doesn't get a login shell's PATH, and ~/.local/bin is
    // where a build of one's own lives.
    QString command = "PATH=\"$HOME/.local/bin:$PATH\" omabook";
    for (const QString &argument : arguments)
        command += " " + quote(argument);
    if (!m_folder.isEmpty())
        command += " --library " + quote(m_folder);
    return run(command);
}

bool Remote::fetch(const QString &remoteFile, const QString &localFile) const {
    return run("cat " + quote(remoteFile), {}, localFile).ok();
}

bool Remote::send(const QString &localFile, const QString &remoteFile, QString *error) const {
    if (!QFileInfo(localFile).isReadable()) {
        if (error)
            *error = QFileInfo(localFile).fileName() + " can't be read here";
        return false;
    }
    const Result sent = run("cat > " + quote(remoteFile), localFile);
    if (!sent.ok() && error)
        *error = sent.error.isEmpty() ? QString("copying %1 stopped").arg(QFileInfo(localFile).fileName()) : sent.error.section('\n', -1);
    return sent.ok();
}

// The shape of the manifest two computers exchange. It goes up when a change
// would make an older Omabook misread a newer one's; an older Omabook then
// refuses to sync and says to update, instead of acting on what it misread.
constexpr int manifestVersion = 1;

QJsonObject libraryManifest(const Library &library) {
    QJsonArray books;
    for (const Book &book : library.books()) {
        QJsonObject object = book.toJson();
        QJsonArray files;
        for (const FormatFile &file : library.formatFiles(book.id))
            files << QJsonObject{{"format", file.format}, {"path", file.path}, {"hash", file.hash}, {"size", file.size}};
        object.insert("files", files);
        object.insert("cover", library.coverPath(book.id));
        object.insert("progress", toJson(library.progress(book.id)));
        // What is said of its reading, which a book without a file is all of.
        object.insert("shelf", book.shelf);
        object.insert("paper", book.paper);
        books << object;
    }
    // An Omabook from before collections reads past them.
    return {{"version", manifestVersion}, {"omabook", QStringLiteral(OMABOOK_VERSION)}, {"books", books},
            {"removed", QJsonArray::fromStringList(library.removedHashes())}, {"collections", Collections::manifest(library)}};
}

SyncSummary syncLibrary(Library &library, const Remote &remote, const std::function<void(const QString &)> &progress) {
    SyncSummary summary;
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

    // What they have: fetch the books this library lacks, and for the ones
    // both have, the formats it lacks and the newer reading place.
    for (const QJsonValue &value : manifest.value("books").toArray()) {
        const QJsonObject theirs = value.toObject();
        const qint64 theirId = theirs.value("id").toInteger();
        const QJsonArray files = theirs.value("files").toArray();
        const Progress theirPlace = progressFrom(theirs.value("progress").toObject());
        qint64 mine = 0;
        bool removedHere = false;
        for (const QJsonValue &file : files) {
            const QString hash = file.toObject().value("hash").toString();
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
        if (!mine && removedHere)
            continue;
        if (progress && !mine)
            progress(theirs.value("title").toString());

        auto fetched = [&](const QJsonObject &file) -> QString {
            const QString local = scratch.filePath(QString("%1.%2").arg(++counter).arg(file.value("format").toString()));
            if (!remote.fetch(file.value("path").toString(), local)
                || Library::hashFile(local) != file.value("hash").toString()) {
                summary.problems << theirs.value("title").toString() + " did not arrive whole from " + remote.host() + ".";
                QFile::remove(local);
                return {};
            }
            return local;
        };

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
            fillReading(library, added.id, theirs);
            if (const std::optional<Book> made = library.book(added.id))
                for (const QString &key : Library::entryKeys(*made))
                    myKeys.insert(key, added.id);
            ++summary.received;
            continue;
        }
        if (!mine) {
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
            ++summary.received;
        }
        fillReading(library, mine, theirs);
        // Older Omabooks don't say "shelf"; they are given nothing they can't keep.
        if (const std::optional<Book> merged = library.book(mine); merged && theirs.contains("shelf") && saysLess(theirs, *merged))
            remote.omabook(readingArguments(theirId, *merged));
        if (files.isEmpty())
            continue;
        const QStringList have = library.book(mine)->formats;
        for (const QJsonValue &file : files) {
            if (have.contains(file.toObject().value("format").toString()))
                continue;
            const QString local = fetched(file.toObject());
            QString error;
            if (!local.isEmpty() && !library.addFormat(mine, local, &error))
                summary.problems << error;
            QFile::remove(local);
        }

        const Progress myPlace = library.progress(mine);
        if (theirPlace.updated > myPlace.updated) {
            library.setProgress(mine, theirPlace);
            ++summary.placesTaken;
        } else if (myPlace.updated > theirPlace.updated) {
            if (remote.omabook(progressArguments(theirId, myPlace)).ok())
                ++summary.placesSent;
        }
    }

    // What they lack: send it, unless they removed it themselves.
    QSet<QString> removedThere;
    for (const QJsonValue &hash : manifest.value("removed").toArray())
        removedThere << hash.toString();
    for (const Book &book : library.books()) {
        const QList<FormatFile> files = library.formatFiles(book.id);
        bool known = false, removed = false;
        for (const FormatFile &file : files) {
            known = known || theirHashes.contains(file.hash);
            removed = removed || removedThere.contains(file.hash);
        }
        if (files.isEmpty())
            for (const QString &key : Library::entryKeys(book)) {
                known = known || theirKeys.contains(key);
                removed = removed || removedThere.contains(key);
            }
        if (known || removed)
            continue;
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
            if (why.isEmpty() && !remote.send(file.path, target, &why))
                why = file.format.toUpper() + ": " + why;
            sentFiles << target;
        }
        const QString details = scratch.filePath("details.json");
        QFile json(details);
        if (why.isEmpty() && !(json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(book.meta.toJson()).toJson()) > 0))
            why = "its details can't be written here";
        json.close();
        if (why.isEmpty())
            remote.send(details, folder + "/details.json", &why);
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
        remote.omabook(readingArguments(theirId, book));
        ++summary.sent;
    }
    // The collections, once both sides have every book: theirs taken in here
    // where they changed last, and this library's handed to them to do the
    // same. An Omabook from before collections has none to give, and can't
    // take any; the books are in step all the same.
    if (manifest.contains("collections")) {
        summary.collectionsTaken = Collections::take(library, manifest.value("collections").toArray());
        const QJsonArray mine = Collections::manifest(library);
        const QString file = scratch.filePath("collections.json");
        QFile json(file);
        if (!mine.isEmpty() && json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(mine).toJson(QJsonDocument::Compact)) > 0) {
            json.close();
            const QString folder = QString::fromUtf8(remote.run("mktemp -d").output).trimmed();
            if (!folder.isEmpty()) {
                const auto cleanup = qScopeGuard([&] { remote.run("rm -rf " + Remote::quote(folder)); });
                const Remote::Result taken = remote.send(file, folder + "/collections.json")
                                                 ? remote.omabook({"collections", "take", folder + "/collections.json", "--json"})
                                                 : Remote::Result();
                if (taken.ok())
                    summary.collectionsSent = int(QJsonDocument::fromJson(taken.output).object().value("changed").toInt());
                else
                    summary.problems << "The collections could not be sent to " + remote.host() + ".";
            }
        }
    }
    qCInfo(lcSync).noquote() << "with" << remote.host() << ":" << summary.received << "received," << summary.sent << "sent,"
                             << summary.placesTaken + summary.placesSent << "reading places," << summary.problems.size() << "problems";
    return summary;
}
