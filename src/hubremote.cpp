#include "hubremote.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <tuple>
#include <utility>

#include "hublinks.h"
#include "hubsync.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

HubRemote::HubRemote(const QString &target, Ask ask)
    : Remote(target), m_ask(std::move(ask)), m_library(target.contains(u':') ? target.section(u':', 1).trimmed() : QString()) {}

HubRemote::~HubRemote() { end(); }

std::expected<QJsonObject, Failure> HubRemote::step(QJsonObject request) const {
    request.insert("request"_L1, u"sync"_s);
    if (!m_visit.isEmpty())
        request.insert("visit"_L1, m_visit);
    const QByteArray answered = m_ask(QJsonDocument(request).toJson(QJsonDocument::Compact));
    QJsonObject answer = QJsonDocument::fromJson(answered).object();
    if (answer.isEmpty())
        return std::unexpected(Failure(Failure::Code::HubSessionEnded, {host()}));
    // A library not paired with the hub's is said as here (#217): worded in the window's language, and pairable.
    if (answer.contains("unpaired"_L1))
        return std::unexpected(Failure(Failure::Code::HubSyncNotPaired, {answer.value("unpaired"_L1).toString()}));
    if (!answer.value("ok"_L1).toBool())
        return std::unexpected(Failure::said(answer.value("error"_L1).toString()));
    return answer;
}

std::expected<void, Failure> HubRemote::begin(const Library &library, bool dryRun, bool pair) {
    const auto begun = step({{"step", "begin"},
                             {"library", m_library},
                             {"format", library.formatVersion()},
                             {"dry_run", dryRun},
                             {"device", Spaces::deviceId(library)},
                             {"pair", pair}});
    if (!begun)
        return std::unexpected(begun.error());
    m_visit = begun->value("visit"_L1).toString();
    return {};
}

void HubRemote::end() {
    if (m_visit.isEmpty())
        return;
    std::ignore = step({{"step", "end"}}); // over either way: the hub lets an idle one go
    m_visit.clear();
}

Remote::Result HubRemote::omabook(const QStringList &arguments, [[maybe_unused]] const QString &inputFile) const {
    const auto answered = step({{"step", "omabook"}, {"arguments", QJsonArray::fromStringList(arguments)}});
    if (!answered)
        return {.code = 1, .output = {}, .error = answered.error().text()};
    const QJsonValue answer = answered->value("answer"_L1);
    return {.code = 0,
            .output = answer.isArray() ? QJsonDocument(answer.toArray()).toJson() : QJsonDocument(answer.toObject()).toJson(),
            .error = {}};
}

Remote::Result HubRemote::run(const QString &command, [[maybe_unused]] const QString &inputFile,
                              [[maybe_unused]] const QString &outputFile) const {
    return {.code = -1, .output = {}, .error = Failure(Failure::Code::HubSyncRefused, {command.left(120)}).text()};
}

bool HubRemote::fetch(const QString &remoteFile, const QString &localFile) const {
    QFile local(localFile);
    if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    qint64 at = 0;
    while (true) {
        const auto piece = step({{"step", "fetch"}, {"file", remoteFile}, {"at", at}});
        const QByteArray data = piece ? QByteArray::fromBase64(piece->value("data"_L1).toString().toLatin1()) : QByteArray();
        if (!piece || local.write(data) != data.size() || (data.isEmpty() && at < piece->value("size"_L1).toInteger())) {
            local.remove();
            return false;
        }
        at += data.size();
        if (at >= piece->value("size"_L1).toInteger())
            return true;
    }
}

std::expected<void, QString> HubRemote::send(const QString &localFile, const QString &remoteFile) const {
    QFile local(localFile);
    if (!local.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::FileUnreadable, {local.errorString()}).text());
    // Every file is put once at least, so an empty one arrives too.
    for (qint64 at = 0; at == 0 || !local.atEnd();) {
        const QByteArray data = local.read(HubSync::piece);
        const auto put = step({{"step", "put"}, {"file", remoteFile}, {"at", at}, {"data", QString::fromLatin1(data.toBase64())}});
        if (!put)
            return std::unexpected(put.error().text());
        at += data.size();
        if (data.isEmpty())
            break;
    }
    return {};
}

QString HubRemote::makeFolder() const {
    const auto made = step({{"step", "folder"}});
    return made ? made->value("folder"_L1).toString() : QString();
}

void HubRemote::dropFolder(const QString &folder) const {
    std::ignore = step({{"step", "drop"}, {"folder", folder}}); // the hub drops it with the sync, if not now
}

SyncSummary HubRemote::sync(Library &library, const QString &target, const Ask &ask, const Progress &progress, const SyncOptions &options) {
    // A session that ended before the sync did is a hub gone out of reach, not one that refused.
    bool ended = false;
    HubRemote hub(target, [&ask, &ended](const QByteArray &line) {
        QByteArray answer = ask(line);
        ended = ended || answer.isEmpty();
        return answer;
    });
    if (const auto begun = hub.begin(library, options.dryRun, options.pair); !begun) {
        SyncSummary refused;
        refused.dryRun = options.dryRun;
        refused.error = begun.error().text();
        if (begun.error().code() == Failure::Code::HubSyncNotPaired) {
            refused.unpairedThere = begun.error().arguments().value(0);
            refused.unpairedHere = QFileInfo(library.root()).fileName();
        }
        refused.unreached = ended;
        return refused;
    }
    SyncSummary summary = syncLibrary(library, hub, progress, options);
    hub.end();
    summary.unreached = ended && !summary.error.isEmpty();
    return summary;
}

SyncSummary HubRemote::syncWith(Library &library, const QString &target, const Progress &progress, const SyncOptions &options,
                                const QStringList &ssh) {
    const HubLinks links;
    const QString host = Remote(target).host();
    if (!links.isLinked(host))
        return syncLibrary(library, Remote(target, ssh), progress, options);
    auto session = links.open(host);
    if (!session) {
        SyncSummary unreached;
        unreached.dryRun = options.dryRun;
        unreached.error = session.error().text();
        unreached.unreached = session.error().code() != Failure::Code::HubNotLinked;
        return unreached;
    }
    return sync(library, target, [&session](const QByteArray &line) { return (*session)->ask(line); }, progress, options);
}
