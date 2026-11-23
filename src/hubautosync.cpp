#include "hubautosync.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QSaveFile>
#include <algorithm>
#include <tuple>
#include <utility>

#include "sync.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QDateTime timeOf(const QJsonValue &value) { return QDateTime::fromString(value.toString(), Qt::ISODate); }
[[nodiscard]] QString textOf(const QDateTime &time) { return time.isValid() ? time.toUTC().toString(Qt::ISODate) : QString(); }

} // namespace

HubAutoSync::HubAutoSync(QString folder) : HubAutoSync(std::move(folder), Times{}) {}

HubAutoSync::HubAutoSync(QString folder, Times times) : m_folder(std::move(folder)), m_times(times) {}

HubAutoSync::~HubAutoSync() = default;

std::map<QString, HubAutoSync::Hub> HubAutoSync::read() const {
    QFile file(QDir(m_folder).filePath(u"autosync.json"_s));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    std::map<QString, Hub> hubs;
    const QJsonObject kept = QJsonDocument::fromJson(file.readAll()).object();
    for (const auto &[host, value] : kept.asKeyValueRange()) {
        const QJsonObject hub = value.toObject();
        hubs[host.toString()] = Hub{.on = hub.value("on"_L1).toBool(),
                                    .last = timeOf(hub.value("last"_L1)),
                                    .refused = hub.value("refused"_L1).toString(),
                                    .told = hub.value("told"_L1).toBool(),
                                    .unreached = hub.value("unreached"_L1).toBool(),
                                    .tried = timeOf(hub.value("tried"_L1))};
    }
    return hubs;
}

std::expected<void, Failure> HubAutoSync::write(const std::map<QString, Hub> &hubs) const {
    QJsonObject kept;
    for (const auto &[host, hub] : hubs)
        kept.insert(host, QJsonObject{{"on", hub.on},
                                      {"last", textOf(hub.last)},
                                      {"refused", hub.refused},
                                      {"told", hub.told},
                                      {"unreached", hub.unreached},
                                      {"tried", textOf(hub.tried)}});
    QSaveFile file(QDir(m_folder).filePath(u"autosync.json"_s));
    if (!QDir().mkpath(m_folder) || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(kept).toJson()) < 0 || !file.commit())
        return std::unexpected(Failure(Failure::Code::BookFileUnwritable, {file.fileName(), file.errorString()}));
    return {};
}

HubAutoSync::Hub HubAutoSync::hub(const QString &host) const {
    const auto hubs = read();
    const auto found = hubs.find(host);
    return found == hubs.end() ? Hub{} : found->second;
}

std::expected<void, Failure> HubAutoSync::turn(const QString &host, bool on) {
    auto hubs = read();
    Hub &hub = hubs[host];
    // Switched on, it syncs at once.
    if (on && !hub.on)
        hub.last = {};
    hub.on = on;
    if (on) {
        hub.refused.clear();
        hub.told = false;
        hub.unreached = false;
    }
    return write(hubs);
}

void HubAutoSync::changed(const QDateTime &now) { m_changed = now; }

void HubAutoSync::touched(const QDateTime &now) { m_touched = now; }

void HubAutoSync::cameIntoReach() { m_reachAgain = true; }

bool HubAutoSync::dueOne(const Hub &hub, const QDateTime &now) const {
    if (!hub.on || !hub.refused.isEmpty())
        return false;
    if (hub.unreached)
        return m_reachAgain || !hub.tried.isValid() || now >= hub.tried.addSecs(m_times.again.count());
    if (!hub.last.isValid() || now >= hub.last.addSecs(m_times.every.count()))
        return true;
    if (!m_changed || *m_changed < hub.last)
        return false;
    const QDateTime quiet = m_touched.isValid() ? std::max(*m_changed, m_touched) : *m_changed;
    return now >= quiet.addSecs(m_times.settle.count());
}

std::optional<QString> HubAutoSync::due(const QStringList &hosts, const QDateTime &now) const {
    const auto hubs = read();
    for (const QString &host : hosts)
        if (const auto found = hubs.find(host); found != hubs.end() && dueOne(found->second, now))
            return host;
    return std::nullopt;
}

void HubAutoSync::began(const QDateTime &now) { m_began = now; }

std::optional<QString> HubAutoSync::done(const QString &host, const SyncSummary &summary, const QDateTime &now) {
    auto hubs = read();
    Hub &hub = hubs[host];
    m_reachAgain = false;
    std::optional<QString> tell;
    if (summary.unreached) {
        hub.unreached = true;
        hub.tried = now;
    } else if (!summary.error.isEmpty()) {
        hub.unreached = false;
        hub.refused = refusal(summary);
        if (!std::exchange(hub.told, true))
            tell = hub.refused;
    } else {
        hub.unreached = false;
        hub.last = m_began.isValid() ? m_began : now;
    }
    std::ignore = write(hubs); // Not kept, it is tried again: nothing worse.
    return tell;
}

void HubAutoSync::byHand(const QString &host, const QDateTime &began) {
    auto hubs = read();
    const auto found = hubs.find(host);
    if (found == hubs.end())
        return;
    Hub &hub = found->second;
    hub.last = began;
    hub.refused.clear();
    hub.told = false;
    hub.unreached = false;
    std::ignore = write(hubs); // As above.
}

std::unique_ptr<QLockFile> HubAutoSync::hold(bool wait) const {
    QDir().mkpath(m_folder);
    auto lock = std::make_unique<QLockFile>(QDir(m_folder).filePath(u"sync.lock"_s));
    // A sync may take long; one that died leaves its lock, which QLockFile sees is stale.
    lock->setStaleLockTime(0);
    if (wait ? lock->lock() : lock->tryLock(0))
        return lock;
    return nullptr;
}

QString HubAutoSync::refusal(const SyncSummary &summary) {
    QStringList said{summary.error};
    for (const QString &breach : summary.breaches)
        said << u"  - "_s + breach;
    return said.join(u'\n');
}

std::pair<QString, QString> HubAutoSync::notice(const QString &host, const QString &refused) {
    return {
        QObject::tr("Omabook stopped syncing with %1 by itself").arg(host),
        QObject::tr("%1\nIt syncs by itself again once a sync by hand goes through, or automatic sync is switched off and on in the Hubs "
                    "panel.")
            .arg(refused)};
}
