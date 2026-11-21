#include "pluginfilesync.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <tuple>

#include "log.h"
#include "pluginfiles.h"
#include "scriptplugins.h"
#include "scripttools.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString folderOf(const QString &plugins) { return plugins.isEmpty() ? ScriptPlugins::defaultFolder() : plugins; }

// The plugins installed in a folder, by their ids.
[[nodiscard]] QStringList installed(const QString &plugins) {
    QStringList ids;
    for (const QString &id : QDir(plugins).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (ScriptTools::validName(id) && QFileInfo::exists(plugins + u'/' + id + u"/plugin.json"_s))
            ids << id;
    return ids;
}

[[nodiscard]] QString dataOf(const QString &plugins, const QString &id) { return plugins + u'/' + id + u"/data"_s; }

// The hash a side's survey says it holds at a path of a plugin's; empty for none.
[[nodiscard]] QString held(const QJsonObject &side, const QString &id, const QString &path) {
    return side["plugins"_L1][id][path]["hash"_L1].toString();
}

[[nodiscard]] bool has(const QJsonObject &side, const QString &id) { return side.value("plugins"_L1).toObject().contains(id); }

struct Sending {
    QString plugin, path, file; // the file here
};

// An on-demand file there lacks, which it fetches from here when its plugin asks (#135).
struct Offered {
    QString plugin, path, hash, source; // where it is here
};

// One sync's carrying of files, between here and there.
struct Exchange {
    const Library *library;
    const Remote *remote;
    QString plugins; // the folder here
    const PluginFileSync::Sides *sides;
    SyncSummary *summary;
    QList<Sending> sending; // what goes there once every plugin was looked at
    QList<Offered> offered; // and what is there to fetch from here
};

// One file here lacks and there holds: fetched at once, or (one the plugin
// marked on demand) remembered where it is there, to fetch when it is asked for.
void takeHere(Exchange &exchange, const QString &id, const QString &path, const QString &hash, bool onDemand,
              PluginFiles::Away &away) {
    const QString source = exchange.sides->there.value("folder"_L1).toString() + u'/' + id + u"/data/"_s + path;
    if (onDemand) {
        away.insert(path, {.hash = hash, .target = exchange.remote->target(), .source = source});
        ++exchange.summary->pluginFilesOnDemand;
        return;
    }
    const QTemporaryDir scratch;
    const QString local = scratch.filePath(u"fetched"_s);
    if (exchange.remote->fetch(source, local) && PluginFiles::take(*exchange.library, id, dataOf(exchange.plugins, id), path, local))
        ++exchange.summary->pluginFilesTaken;
    else
        qCWarning(lcSync).noquote() << "a file of" << id << "did not arrive whole from" << exchange.remote->host();
    QFile::remove(local);
}

// The files of one plugin's that one side lacks and the other holds: taken
// here, and those to send there kept to send; there is told where the
// on-demand ones are here instead.
void exchangeOf(Exchange &exchange, const QString &id) {
    const QString data = dataOf(exchange.plugins, id);
    const PluginFiles::Files known = PluginFiles::recorded(*exchange.library, id);
    const QStringList marked = PluginFiles::onDemand(*exchange.library, id);
    PluginFiles::Away away = PluginFiles::elsewhere(exchange.plugins, id);
    const PluginFiles::Away before = away;
    for (auto file = known.cbegin(); file != known.cend(); ++file) {
        if (file->removed)
            continue;
        const QString here = held(exchange.sides->here, id, file.key());
        const QString there = held(exchange.sides->there, id, file.key());
        const bool onDemand = PluginFiles::isOnDemand(marked, file.key());
        if (here == file->hash)
            away.remove(file.key());
        if (has(exchange.sides->here, id) && here != file->hash && there == file->hash)
            takeHere(exchange, id, file.key(), file->hash, onDemand, away);
        else if (has(exchange.sides->there, id) && there != file->hash && here == file->hash && onDemand)
            exchange.offered.append({id, file.key(), file->hash, QFileInfo(data + u'/' + file.key()).absoluteFilePath()});
        else if (has(exchange.sides->there, id) && there != file->hash && here == file->hash)
            exchange.sending.append({id, file.key(), data + u'/' + file.key()});
    }
    if (away != before)
        PluginFiles::setElsewhere(exchange.plugins, id, away);
    PluginFiles::settle(*exchange.library, id, data);
}

// Sends the files there lacks, and has it take them and remove what was
// removed, and remember where the on-demand ones it lacks are here.
void send(const Remote &remote, const QList<Sending> &sending, const QList<Offered> &offered, SyncSummary &summary) {
    const QString folder = QString::fromUtf8(remote.run(u"mktemp -d"_s).output).trimmed();
    if (folder.isEmpty())
        return;
    const auto cleanup = qScopeGuard([&] { remote.run(u"rm -rf "_s + Remote::quote(folder)); });
    QJsonArray files;
    for (const Sending &file : sending) {
        const QString target = folder + u'/' + QString::number(files.size());
        if (remote.send(file.file, target))
            files << QJsonObject{{"plugin", file.plugin}, {"path", file.path}, {"file", target}};
    }
    QJsonArray elsewhere;
    for (const Offered &file : offered)
        elsewhere << QJsonObject{{"plugin", file.plugin}, {"path", file.path}, {"hash", file.hash}, {"source", file.source}};
    const auto answer =
        remote.exchange(QJsonDocument(QJsonObject{{"files", files}, {"elsewhere", elsewhere}}), {u"sync"_s, u"--take-plugin-files"_s});
    if (answer)
        summary.pluginFilesSent += answer->value("taken"_L1).toInt();
    else
        qCWarning(lcSync).noquote() << "plugins' files were not taken on" << remote.host() << ":" << answer.error();
}

} // namespace

QJsonObject PluginFileSync::survey(const Library &library, const QString &plugins) {
    const QString folder = folderOf(plugins);
    QJsonObject each;
    int over = 0;
    for (const QString &id : installed(folder)) {
        const PluginFiles::Survey found = PluginFiles::survey(dataOf(folder, id));
        std::ignore = PluginFiles::note(library, id, found.files); // a file not recorded now is recorded at the next sync
        QJsonObject files;
        for (auto file = found.files.cbegin(); file != found.files.cend(); ++file)
            files.insert(file.key(), QJsonObject{{"hash", file->hash}, {"size", file->size}});
        each.insert(id, files);
        over += static_cast<int>(found.held.size());
    }
    return {{"folder", QFileInfo(folder).absoluteFilePath()}, {"plugins", each}, {"held", over}};
}

QJsonObject PluginFileSync::take(const Library &library, const QJsonObject &request, const QString &plugins) {
    const QString folder = folderOf(plugins);
    const QStringList ids = installed(folder);
    int taken = 0;
    int removed = 0;
    for (const auto &value : request.value("files"_L1).toArray()) {
        const QString id = value["plugin"_L1].toString();
        if (ids.contains(id)
            && PluginFiles::take(library, id, dataOf(folder, id), value["path"_L1].toString(), value["file"_L1].toString()))
            ++taken;
    }
    for (const QString &id : ids)
        removed += PluginFiles::settle(library, id, dataOf(folder, id));
    // What is on the computer that syncs with this one, to fetch from there (#135).
    QHash<QString, PluginFiles::Away> away;
    for (const auto &value : request.value("elsewhere"_L1).toArray()) {
        const QString id = value["plugin"_L1].toString();
        if (!ids.contains(id))
            continue;
        if (!away.contains(id))
            away.insert(id, PluginFiles::elsewhere(folder, id));
        away[id].insert(value["path"_L1].toString(), {.hash = value["hash"_L1].toString(), .target = {}, .source = value["source"_L1].toString()});
    }
    for (auto it = away.cbegin(); it != away.cend(); ++it)
        PluginFiles::setElsewhere(folder, it.key(), *it);
    return {{"taken", taken}, {"removed", removed}, {"on_demand", static_cast<int>(request.value("elsewhere"_L1).toArray().size())}};
}

std::optional<PluginFileSync::Sides> PluginFileSync::begin(const Library &library, const Remote &remote, const QString &plugins) {
    // An Omabook from before plugins' files travelled refuses the option (exit code 1).
    const Remote::Result there = remote.omabook({u"sync"_s, u"--plugin-files"_s});
    const QJsonObject answer = QJsonDocument::fromJson(there.output).object();
    if (!there.ok() || !answer.contains("plugins"_L1))
        return std::nullopt;
    return Sides{.here = survey(library, plugins), .there = answer};
}

void PluginFileSync::finish(const Library &library, const Remote &remote, const QString &plugins, const Sides &sides,
                            SyncSummary &summary) {
    const QString folder = folderOf(plugins);
    summary.pluginFilesHeld += sides.here.value("held"_L1).toInt() + sides.there.value("held"_L1).toInt();
    QStringList ids = sides.here.value("plugins"_L1).toObject().keys() + sides.there.value("plugins"_L1).toObject().keys();
    ids.removeDuplicates();
    Exchange exchange{.library = &library, .remote = &remote, .plugins = folder, .sides = &sides, .summary = &summary, .sending = {}};
    for (const QString &id : std::as_const(ids))
        exchangeOf(exchange, id);
    // There removes what was removed even when nothing goes to it.
    if (!sides.there.value("plugins"_L1).toObject().isEmpty())
        send(remote, exchange.sending, exchange.offered, summary);
}

PluginFileSync::Fetched PluginFileSync::fetch(const Library &library, const QString &plugins, const QString &plugin, const QString &path,
                                              const QString &syncTarget, const QStringList &ssh) {
    const QString folder = folderOf(plugins);
    const QString data = dataOf(folder, plugin);
    const PluginFiles::File known = PluginFiles::recorded(library, plugin).value(path);
    const QString here = PluginFiles::inside(data, path);
    if (!known.hash.isEmpty() && !here.isEmpty() && QFileInfo(here).isFile() && Library::hashFile(here) == known.hash)
        return Fetched::Here;
    PluginFiles::Away away = PluginFiles::elsewhere(folder, plugin);
    const auto where = away.constFind(path);
    const QString target = where == away.cend() ? QString() : where->target.isEmpty() ? syncTarget : where->target;
    if (known.hash.isEmpty() || known.removed || target.isEmpty())
        return Fetched::Nowhere;
    const QTemporaryDir scratch;
    const QString local = scratch.filePath(u"fetched"_s);
    if (!Remote(target, ssh).fetch(where->source, local) || !PluginFiles::take(library, plugin, data, path, local))
        return Fetched::Failed;
    away.remove(path);
    PluginFiles::setElsewhere(folder, plugin, away);
    return Fetched::Fetched;
}
