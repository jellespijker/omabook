#include "sync.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>

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

bool Remote::send(const QString &localFile, const QString &remoteFile) const {
    return run("cat > " + quote(remoteFile), localFile).ok();
}

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
        books << object;
    }
    return {{"version", 1}, {"books", books}, {"removed", QJsonArray::fromStringList(library.removedHashes())}};
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
    QTemporaryDir scratch;
    int counter = 0;
    QSet<QString> theirHashes;

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

        if (!mine) {
            if (files.isEmpty())
                continue;
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
        if (known || removed || files.isEmpty())
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
        bool whole = true;
        for (const FormatFile &file : files) {
            const QString target = folder + "/book." + file.format;
            whole = whole && remote.send(file.path, target);
            sentFiles << target;
        }
        const QString details = scratch.filePath("details.json");
        QFile json(details);
        whole = whole && json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(book.meta.toJson()).toJson()) > 0;
        json.close();
        whole = whole && remote.send(details, folder + "/details.json");
        QStringList add{"add", sentFiles.value(0), "--meta", folder + "/details.json", "--json"};
        if (const QString cover = library.coverPath(book.id); !cover.isEmpty() && remote.send(cover, folder + "/cover.jpg"))
            add << "--cover" << folder + "/cover.jpg";
        const Remote::Result added = whole ? remote.omabook(add) : Remote::Result();
        const qint64 theirId = QJsonDocument::fromJson(added.output).array().first().toObject().value("id").toInteger();
        if (!added.ok() || theirId <= 0) {
            summary.problems << book.meta.title + " could not be sent to " + remote.host() + "."
                                    + (added.error.isEmpty() ? QString() : " " + added.error.section('\n', -1));
            continue;
        }
        for (const QString &file : sentFiles.mid(1))
            remote.omabook({"add", file, "--to", QString::number(theirId)});
        if (const Progress place = library.progress(book.id); place.updated > 0)
            remote.omabook(progressArguments(theirId, place));
        ++summary.sent;
    }
    return summary;
}
