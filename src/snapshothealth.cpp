#include "snapshothealth.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStorageInfo>
#include <algorithm>
#include <utility>

#include "log.h"

using namespace Qt::StringLiterals;
using SnapshotWarnings::Medium;
using SnapshotWarnings::Outcome;

namespace {

// Where a hub keeps its snapshots' password when systemd's credentials don't (HubPlace::secret).
constexpr auto hubSecret = "/secrets/snapshots"_L1;

[[nodiscard]] QString timeText(const QDateTime &time) { return time.isValid() ? time.toString(Qt::ISODateWithMs) : QString(); }
[[nodiscard]] QDateTime timeOf(const QJsonValue &value) { return QDateTime::fromString(value.toString(), Qt::ISODateWithMs); }

[[nodiscard]] QJsonObject outcomeJson(const Outcome &outcome) {
    return {{u"ran"_s, timeText(outcome.ran)}, {u"done"_s, outcome.done}, {u"said"_s, outcome.said}};
}
[[nodiscard]] Outcome outcomeOf(const QJsonValue &value) {
    const QJsonObject kept = value.toObject();
    return {.ran = timeOf(kept.value("ran"_L1)), .done = kept.value("done"_L1).toBool(), .said = kept.value("said"_L1).toString()};
}
[[nodiscard]] Outcome outcomeOf(const std::expected<void, Failure> &done, const QDateTime &now) {
    return {.ran = now, .done = done.has_value(), .said = done ? QString() : done.error().english()};
}

// The record of one repository, by its name ("" for the computer's own), first known now.
[[nodiscard]] QJsonObject recordOf(const QJsonObject &record, const QString &name, const QDateTime &now) {
    QJsonObject repository = record.value("repositories"_L1).toObject().value(name).toObject();
    if (!repository.contains("known"_L1))
        repository.insert(u"known"_s, timeText(now));
    return repository;
}
void keepRecord(QJsonObject &record, const QString &name, const QJsonObject &repository) {
    QJsonObject repositories = record.value("repositories"_L1).toObject();
    repositories.insert(name, repository);
    record.insert(u"repositories"_s, repositories);
}

// The room on the disk of a repository kept in a folder here.
void roomOf(const QString &location, QJsonObject &repository) {
    if (!QDir::isAbsolutePath(location))
        return;
    QString folder = location;
    while (!QFileInfo::exists(folder) && QFileInfo(folder).absolutePath() != folder)
        folder = QFileInfo(folder).absolutePath();
    const QStorageInfo disk(folder);
    if (!disk.isValid())
        return;
    repository.insert(u"free"_s, disk.bytesAvailable());
    repository.insert(u"total"_s, disk.bytesTotal());
}

// Each library's newest snapshot, by its tag.
[[nodiscard]] QJsonObject newestOf(const QList<Restic::Snapshot> &snapshots) {
    QJsonObject newest;
    for (const Restic::Snapshot &snapshot : snapshots)
        if (const QString tag = snapshot.tagged(u"library:"_s); !tag.isEmpty())
            newest.insert(u"library:"_s + tag, timeText(snapshot.time)); // oldest first, so the last stays
    return newest;
}

[[nodiscard]] SnapshotWarnings::Repository repositoryOf(const SnapshotHealth::Target &target, const QJsonObject &kept) {
    SnapshotWarnings::Repository repository;
    repository.name = target.name;
    repository.medium = target.medium;
    repository.location = target.location;
    if (!target.repository && !target.passwordAt.isEmpty())
        repository.passwordAt = target.passwordAt;
    else if (!target.repository && !target.absent)
        repository.unreadable = target.repository.error().english();
    else
        repository.unreadable = kept.value("unreadable"_L1).toString();
    repository.absent = target.absent;
    repository.known = timeOf(kept.value("known"_L1));
    repository.free = kept.value("free"_L1).toInteger(-1);
    repository.total = kept.value("total"_L1).toInteger(-1);
    repository.snapshot = target.snapshot.value_or(outcomeOf(kept.value("snapshot"_L1)));
    repository.check = outcomeOf(kept.value("check"_L1));
    repository.dataCheck = outcomeOf(kept.value("data_check"_L1));
    repository.checked = timeOf(kept.value("checked"_L1));
    repository.dataChecked = timeOf(kept.value("data_checked"_L1));
    const QJsonObject newest = kept.value("newest"_L1).toObject();
    for (auto one = newest.begin(); one != newest.end(); ++one)
        repository.newest[one.key()] = timeOf(one.value());
    return repository;
}

[[nodiscard]] QJsonObject repositoryJson(const SnapshotWarnings::Repository &repository) {
    QDateTime newest;
    for (const auto &[tag, time] : repository.newest)
        newest = std::max(newest, time);
    return {{u"name"_s, repository.name},
            {u"medium"_s, SnapshotWarnings::mediumName(repository.medium)},
            {u"location"_s, repository.location},
            {u"absent"_s, repository.absent},
            {u"newest"_s, timeText(newest)},
            {u"snapshot"_s, outcomeJson(repository.snapshot)},
            {u"check"_s, outcomeJson(repository.check)},
            {u"data_check"_s, outcomeJson(repository.dataCheck)},
            {u"free"_s, repository.free},
            {u"total"_s, repository.total}};
}

} // namespace

SnapshotHealth::SnapshotHealth(Snapshots snapshots, Targets destinations)
    : m_snapshots(std::move(snapshots)), m_destinations(std::move(destinations)), m_file(m_snapshots.place().state + u"/health.json"_s) {}

bool SnapshotHealth::chosen() const { return !m_snapshots.place().repository.location.isEmpty(); }

QList<SnapshotHealth::Target> SnapshotHealth::targets() const {
    if (!chosen())
        return {};
    const SnapshotPlace &place = m_snapshots.place();
    const QString passwordAt = place.hub ? place.hub->state() + hubSecret : place.repository.passwordFile;
    std::expected<ResticRepository, Failure> repository = place.repository;
    if (!m_snapshots.ready())
        repository = std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    QList<Target> targets{{.name = {},
                           .medium = Medium::Own,
                           .location = place.repository.location,
                           .repository = std::move(repository),
                           .passwordAt = passwordAt,
                           .absent = false,
                           .snapshot = std::nullopt}};
    if (m_destinations)
        targets << m_destinations();
    return targets;
}

void SnapshotHealth::noteSnapshot(const std::expected<void, Failure> &outcome, const QDateTime &now) const {
    QJsonObject record = read();
    QJsonObject own = recordOf(record, {}, now);
    own.insert(u"snapshot"_s, outcomeJson(outcomeOf(outcome, now)));
    keepRecord(record, {}, own);
    write(record);
}

void SnapshotHealth::check(Depth depth, const QDateTime &now) const {
    QJsonObject record = read();
    const bool data = depth == Depth::Data;
    for (const Target &target : targets()) {
        QJsonObject kept = recordOf(record, target.name, now);
        if (target.repository && !target.absent) {
            const QString readData = data ? readDataOf(target.medium, now) : QString();
            const auto checked = Restic::check(*target.repository, readData);
            kept.insert(data ? u"data_check"_s : u"check"_s, outcomeJson(outcomeOf(checked, now)));
            if (checked)
                kept.insert(data ? u"data_checked"_s : u"checked"_s, timeText(now));
            else
                qCWarning(lcLibrary).noquote() << "The check of the snapshots in" << target.location
                                               << "failed:" << checked.error().english();
        }
        keepRecord(record, target.name, kept);
    }
    write(record);
}

void SnapshotHealth::look(const QDateTime &now) const {
    QJsonObject record = read();
    for (const Target &target : targets()) {
        QJsonObject kept = recordOf(record, target.name, now);
        if (target.repository && !target.absent) {
            const auto listed = Restic::snapshots(*target.repository, {u"omabook"_s});
            if (listed) {
                kept.insert(u"newest"_s, newestOf(*listed));
                kept.remove(u"unreadable"_s);
            } else {
                kept.insert(u"unreadable"_s, listed.error().english());
            }
            roomOf(target.location, kept);
        }
        keepRecord(record, target.name, kept);
    }
    write(record);
}

QList<SnapshotWarnings::Repository> SnapshotHealth::repositories() const {
    const QJsonObject record = read();
    QList<SnapshotWarnings::Repository> repositories;
    for (const Target &target : targets())
        repositories << repositoryOf(target, record.value("repositories"_L1).toObject().value(target.name).toObject());
    return repositories;
}

QList<SnapshotWarnings::Warning> SnapshotHealth::warnings(const QList<std::pair<QString, QString>> &libraries, const QDateTime &now,
                                                          const SnapshotWarnings::Thresholds &thresholds) const {
    if (!chosen())
        return {};
    QJsonObject record = read();
    QJsonObject seen = record.value("libraries"_L1).toObject();
    QList<SnapshotWarnings::Library> known;
    for (const auto &[tag, name] : libraries) {
        if (!seen.contains(tag))
            seen.insert(tag, timeText(now));
        known << SnapshotWarnings::Library{.tag = tag, .name = name, .seen = timeOf(seen.value(tag))};
    }
    if (seen != record.value("libraries"_L1).toObject()) {
        record.insert(u"libraries"_s, seen);
        write(record);
    }
    return SnapshotWarnings::of(repositories(), known, now, thresholds);
}

QList<SnapshotWarnings::Warning> SnapshotHealth::toTell(const QList<SnapshotWarnings::Warning> &warnings, const QDateTime &now,
                                                        std::chrono::days remindAfter) const {
    QJsonObject record = read();
    const QJsonObject told = record.value("told"_L1).toObject();
    QJsonObject standing;
    QList<SnapshotWarnings::Warning> tell;
    for (const auto &warning : warnings) {
        const QDateTime last = timeOf(told.value(warning.key()));
        const bool due = !last.isValid() || last.secsTo(now) >= std::chrono::duration_cast<std::chrono::seconds>(remindAfter).count();
        if (due)
            tell << warning;
        standing.insert(warning.key(), timeText(due ? now : last));
    }
    record.insert(u"told"_s, standing);
    write(record);
    return tell;
}

QJsonObject SnapshotHealth::json(const QList<SnapshotWarnings::Warning> &warnings) const {
    QJsonArray repositories;
    for (const auto &repository : this->repositories())
        repositories << repositoryJson(repository);
    QJsonArray said;
    for (const auto &warning : warnings)
        said << SnapshotWarnings::json(warning);
    return {{u"set_up"_s, chosen()}, {u"fine"_s, warnings.isEmpty()}, {u"repositories"_s, repositories}, {u"warnings"_s, said}};
}

QString SnapshotHealth::readDataOf(Medium medium, const QDateTime &now) {
    return medium == Medium::Away ? u"%1/12"_s.arg(now.date().month()) : u"all"_s;
}

QJsonObject SnapshotHealth::read() const {
    QFile file(m_file);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

void SnapshotHealth::write(const QJsonObject &record) const {
    QSaveFile file(m_file);
    if (!QDir().mkpath(QFileInfo(m_file).absolutePath()) || !file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(record).toJson()) < 0 || !file.commit())
        qCWarning(lcLibrary).noquote() << "How the snapshots stand can't be kept in" << m_file;
}
