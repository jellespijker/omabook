#include "hubschedule.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <utility>

#include "log.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// The timer wakes at least this often, so a clock put forward (or a computer
// woken from sleep) is noticed within it.
constexpr auto longestSleep = 1h;

} // namespace

HubSchedule::HubSchedule(HubKeeper &keeper, QString stateFolder)
    : m_keeper(keeper), m_file(QDir(stateFolder).filePath(u"schedule.json"_s)) {
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] {
        runDue(QDateTime::currentDateTime());
        wake();
    });
    read();
}

void HubSchedule::daily(const QString &name, QTime at, Job job) { m_jobs.insert_or_assign(name, Planned{.job = std::move(job), .at = at}); }

void HubSchedule::every(const QString &name, std::chrono::seconds interval, Job job) {
    m_jobs.insert_or_assign(name, Planned{.job = std::move(job), .at = std::nullopt, .interval = interval});
}

QDateTime HubSchedule::dueDaily(const QDateTime &last, QTime at, const QDateTime &now) {
    if (!last.isValid())
        return now;
    QDateTime next(last.date(), at);
    if (next <= last)
        next = next.addDays(1);
    return next;
}

QDateTime HubSchedule::dueOf(const QString &name, const Planned &planned, const QDateTime &now) const {
    const auto kept = m_outcomes.find(name);
    if (kept == m_outcomes.end() || !kept->second.ran.isValid())
        return now;
    const Outcome &last = kept->second;
    if (!last.done)
        return last.ran.addSecs(std::chrono::duration_cast<std::chrono::seconds>(retry).count());
    return planned.at ? dueDaily(last.ran, *planned.at, now) : last.ran.addSecs(planned.interval.count());
}

QDateTime HubSchedule::due(const QString &name) const {
    const auto planned = m_jobs.find(name);
    return planned == m_jobs.end() ? QDateTime() : dueOf(name, planned->second, QDateTime::currentDateTime());
}

void HubSchedule::runDue(const QDateTime &now) {
    for (const auto &[name, planned] : m_jobs) {
        if (dueOf(name, planned, now) > now)
            continue;
        qCInfo(lcHub).noquote() << "Running" << name;
        const auto done = planned.job(m_keeper);
        m_outcomes.insert_or_assign(name, Outcome{.ran = now, .done = done.has_value(), .said = done ? QString() : done.error().english()});
        if (!done)
            qCWarning(lcHub).noquote() << name << "failed, and is tried again in an hour:" << done.error().english();
    }
    write();
}

std::optional<HubSchedule::Outcome> HubSchedule::outcome(const QString &name) const {
    const auto kept = m_outcomes.find(name);
    return kept == m_outcomes.end() ? std::nullopt : std::optional(kept->second);
}

void HubSchedule::start() {
    runDue(QDateTime::currentDateTime());
    wake();
}

void HubSchedule::wake() {
    const QDateTime now = QDateTime::currentDateTime();
    QDateTime soonest = now.addSecs(std::chrono::duration_cast<std::chrono::seconds>(longestSleep).count());
    for (const auto &[name, planned] : m_jobs)
        soonest = std::min(soonest, dueOf(name, planned, now));
    m_timer.start(std::max(std::chrono::milliseconds(1s), std::chrono::milliseconds(now.msecsTo(soonest))));
}

void HubSchedule::read() {
    QFile file(m_file);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject jobs = QJsonDocument::fromJson(file.readAll()).object();
    for (auto job = jobs.begin(); job != jobs.end(); ++job) {
        const QJsonObject kept = job.value().toObject();
        m_outcomes.insert_or_assign(job.key(), Outcome{.ran = QDateTime::fromString(kept.value("ran"_L1).toString(), Qt::ISODateWithMs),
                                                       .done = kept.value("done"_L1).toBool(),
                                                       .said = kept.value("said"_L1).toString()});
    }
}

void HubSchedule::write() const {
    QJsonObject jobs;
    for (const auto &[name, outcome] : m_outcomes)
        jobs.insert(name, QJsonObject{{"ran", outcome.ran.toString(Qt::ISODateWithMs)}, {"done", outcome.done}, {"said", outcome.said}});
    QSaveFile file(m_file);
    if (!QDir().mkpath(QFileInfo(m_file).absolutePath()) || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(jobs).toJson()) < 0
        || !file.commit())
        qCWarning(lcHub).noquote() << "When the hub's jobs last ran can't be kept in" << m_file << "; they run again at its next start.";
}
