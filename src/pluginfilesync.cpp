#include "pluginfilesync.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <tuple>

#include "failure.h"
#include "log.h"
#include "pluginfiles.h"
#include "scriptplugins.h"
#include "scripttools.h"
#include "spaces.h"

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

// The plugins whose files a folder keeps: those installed there, or every one with a folder there.
[[nodiscard]] QStringList kept(const QString &plugins, PluginFileSync::Keeping keeping) {
    if (keeping == PluginFileSync::Keeping::Installed)
        return installed(plugins);
    QStringList ids = QDir(plugins).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    ids.removeIf([](const QString &id) { return !ScriptTools::validName(id); });
    return ids;
}

[[nodiscard]] QString dataOf(const QString &plugins, const QString &id) { return plugins + u'/' + id + u"/data"_s; }

// The hash a side's survey says it holds at a path of a plugin's; empty for none.
[[nodiscard]] QString held(const QJsonObject &side, const QString &id, const QString &path) {
    return side["plugins"_L1][id][path]["hash"_L1].toString();
}

// Whether a side keeps every plugin's files: a hub (#212).
[[nodiscard]] bool keepsEvery(const QJsonObject &side) { return side.value("every"_L1).toBool(); }

[[nodiscard]] bool has(const QJsonObject &side, const QString &id) {
    return keepsEvery(side) || side.value("plugins"_L1).toObject().contains(id);
}

// Whether writing what the log says a file holds, nothing, where a side holds something would empty it.
[[nodiscard]] bool empties(const PluginFiles::File &file, const QJsonObject &side, const QString &id, const QString &path) {
    return file.size == 0 && side["plugins"_L1][id][path]["size"_L1].toInteger() > 0;
}

struct Sending {
    QString plugin, path, file; // the file here
};

// An on-demand file there lacks, which it fetches from here when its plugin asks (#135).
struct Offered {
    QString plugin, path, hash, source; // where it is here
};

// Whether taking a file that was sent leaves something where it goes: an empty
// one never takes the place of one that has something in it (#212).
[[nodiscard]] bool notEmptying(const QString &data, const QJsonObject &sent) {
    const QFileInfo target(PluginFiles::inside(data, sent.value("path"_L1).toString()));
    return QFileInfo(sent.value("file"_L1).toString()).size() > 0 || !target.isFile() || target.size() == 0;
}

[[nodiscard]] QString notOwn(const QString &host) { return Failure(Failure::Code::PluginFilesNotOwn, {host}).text(); }

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
void takeHere(const Exchange &exchange, const QString &id, const QString &path, const QString &hash, bool onDemand,
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

// Whether taking a file here (`here`), or sending it there, would empty one that has something in it,
// which a sync never does (#212): said, and the file kept as it is.
[[nodiscard]] bool wouldEmpty(const Exchange &exchange, const QString &id, const QString &path, const PluginFiles::File &file, bool here) {
    if (!empties(file, here ? exchange.sides->here : exchange.sides->there, id, path))
        return false;
    exchange.summary->problems << (here ? Failure(Failure::Code::PluginFileNotEmptiedHere, {path, id})
                                        : Failure(Failure::Code::PluginFileNotEmptiedThere, {path, id, exchange.remote->host()}))
                                      .text();
    return true;
}

// One file of a plugin's that one side lacks and the other holds, as the log
// says it: taken here, or kept to send there, or (on demand) there is told
// where it is here instead. A hub is sent every file whole, and gives them so.
// Never one that would empty a file the other side has something in (#212).
void exchangeFile(Exchange &exchange, const QString &id, const QString &path, const PluginFiles::File &file, bool onDemand,
                  PluginFiles::Away &away) {
    const PluginFileSync::Sides &sides = *exchange.sides;
    const QString here = held(sides.here, id, path);
    const QString there = held(sides.there, id, path);
    if (here == file.hash)
        away.remove(path);
    const bool hereTakes = has(sides.here, id) && here != file.hash && there == file.hash;
    const bool thereTakes = has(sides.there, id) && there != file.hash && here == file.hash;
    if ((hereTakes || thereTakes) && wouldEmpty(exchange, id, path, file, hereTakes))
        return;
    const bool hub = keepsEvery(sides.there);
    const QString data = dataOf(exchange.plugins, id);
    if (hereTakes)
        takeHere(exchange, id, path, file.hash, onDemand && !hub, away);
    else if (thereTakes && onDemand && !hub)
        exchange.offered.append({id, path, file.hash, QFileInfo(data + u'/' + path).absoluteFilePath()});
    else if (thereTakes)
        exchange.sending.append({id, path, data + u'/' + path});
}

// The files of one plugin's that one side lacks and the other holds.
void exchangeOf(Exchange &exchange, const QString &id) {
    const QString data = dataOf(exchange.plugins, id);
    const PluginFiles::Files known = PluginFiles::recorded(*exchange.library, id);
    const QStringList marked = PluginFiles::onDemand(*exchange.library, id);
    PluginFiles::Away away = PluginFiles::elsewhere(exchange.plugins, id);
    const PluginFiles::Away before = away;
    for (auto file = known.cbegin(); file != known.cend(); ++file)
        if (!file->removed)
            exchangeFile(exchange, id, file.key(), *file, PluginFiles::isOnDemand(marked, file.key()), away);
    if (away != before)
        PluginFiles::setElsewhere(exchange.plugins, id, away);
    PluginFiles::settle(*exchange.library, id, data);
}

// Sends the files there lacks, and has it take them and remove what was
// removed, and remember where the on-demand ones it lacks are here.
void send(const Exchange &exchange) {
    const Remote &remote = *exchange.remote;
    SyncSummary &summary = *exchange.summary;
    const QString folder = remote.makeFolder();
    if (folder.isEmpty())
        return;
    const auto cleanup = qScopeGuard([&] { remote.dropFolder(folder); });
    QJsonArray files;
    for (const Sending &file : exchange.sending) {
        const QString target = folder + u'/' + QString::number(files.size());
        if (remote.send(file.file, target))
            files << QJsonObject{{"plugin", file.plugin}, {"path", file.path}, {"file", target}};
    }
    QJsonArray elsewhere;
    for (const Offered &file : exchange.offered)
        elsewhere << QJsonObject{{"plugin", file.plugin}, {"path", file.path}, {"hash", file.hash}, {"source", file.source}};
    const QJsonObject request{{"files", files}, {"elsewhere", elsewhere}, {"device", Spaces::deviceId(*exchange.library)}};
    const auto answer = remote.exchange(QJsonDocument(request), {u"sync"_s, u"--take-plugin-files"_s});
    if (answer && !answer->value("refused"_L1).toBool())
        summary.pluginFilesSent += answer->value("taken"_L1).toInt();
    else if (answer)
        summary.problems << notOwn(remote.host());
    else
        qCWarning(lcSync).noquote() << "plugins' files were not taken on" << remote.host() << ":" << answer.error();
}

} // namespace

QJsonObject PluginFileSync::survey(const Library &library, const QString &plugins, Keeping keeping) {
    const QString folder = folderOf(plugins);
    QJsonObject each;
    int over = 0;
    for (const QString &id : kept(folder, keeping)) {
        const PluginFiles::Survey found = PluginFiles::survey(dataOf(folder, id));
        std::ignore = PluginFiles::note(library, id, found.files); // a file not recorded now is recorded at the next sync
        QJsonObject files;
        for (auto file = found.files.cbegin(); file != found.files.cend(); ++file)
            files.insert(file.key(), QJsonObject{{"hash", file->hash}, {"size", file->size}});
        each.insert(id, files);
        over += static_cast<int>(found.held.size());
    }
    QJsonObject answer{
        {"folder", QFileInfo(folder).absoluteFilePath()}, {"plugins", each}, {"held", over}, {"device", Spaces::deviceId(library)}};
    if (keeping == Keeping::Every)
        answer.insert("every"_L1, true);
    return answer;
}

QJsonObject PluginFileSync::take(const Library &library, const QJsonObject &request, const QString &plugins, Keeping keeping) {
    const QString folder = folderOf(plugins);
    // To one's own computers only, as the plugins' records in the log (#212).
    if (const QString device = request.value("device"_L1).toString();
        device.isEmpty() ? keeping == Keeping::Every : !Spaces::isOwnComputer(library, device))
        return {{"refused", true}};
    const QStringList ids = kept(folder, keeping);
    const auto takes = [&](const QString &id) { return keeping == Keeping::Every ? ScriptTools::validName(id) : ids.contains(id); };
    int taken = 0;
    int removed = 0;
    for (const auto &value : request.value("files"_L1).toArray()) {
        const QString id = value["plugin"_L1].toString();
        if (takes(id) && notEmptying(dataOf(folder, id), value.toObject())
            && PluginFiles::take(library, id, dataOf(folder, id), value["path"_L1].toString(), value["file"_L1].toString()))
            ++taken;
    }
    for (const QString &id : kept(folder, keeping))
        removed += PluginFiles::settle(library, id, dataOf(folder, id));
    // What is on the computer that syncs with this one, to fetch from there (#135); a hub is sent it all.
    QHash<QString, PluginFiles::Away> away;
    for (const auto &value : request.value("elsewhere"_L1).toArray()) {
        const QString id = value["plugin"_L1].toString();
        if (!ids.contains(id) || keeping == Keeping::Every)
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
    // Once the log went, the other side is one of one's own computers, or the files stay where they are (#212).
    if (const QString device = sides.there.value("device"_L1).toString(); !device.isEmpty() && !Spaces::isOwnComputer(library, device)) {
        summary.problems << notOwn(remote.host());
        return;
    }
    const QString folder = folderOf(plugins);
    summary.pluginFilesHeld += sides.here.value("held"_L1).toInt() + sides.there.value("held"_L1).toInt();
    QStringList ids = sides.here.value("plugins"_L1).toObject().keys() + sides.there.value("plugins"_L1).toObject().keys();
    ids.removeDuplicates();
    Exchange exchange{.library = &library, .remote = &remote, .plugins = folder, .sides = &sides, .summary = &summary, .sending = {}};
    for (const QString &id : std::as_const(ids))
        exchangeOf(exchange, id);
    // There removes what was removed even when nothing goes to it; a hub takes what none of its plugins had yet.
    if (!sides.there.value("plugins"_L1).toObject().isEmpty() || keepsEvery(sides.there))
        send(exchange);
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
    // Where it is said to be, or else the computer this one syncs with.
    QString target;
    if (where != away.cend())
        target = where->target.isEmpty() ? syncTarget : where->target;
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
