#include "snapshotwarnings.h"

#include <QLocale>
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

using SnapshotWarnings::Kind;
using SnapshotWarnings::Repository;
using SnapshotWarnings::Thresholds;
using SnapshotWarnings::Warning;

[[nodiscard]] qint64 secondsOf(std::chrono::seconds span) { return span.count(); }

// Whether `since` is longer ago than `span` at `now`.
[[nodiscard]] bool olderThan(const QDateTime &since, std::chrono::seconds span, const QDateTime &now) {
    return since.isValid() && since.secsTo(now) > secondsOf(span);
}

[[nodiscard]] Warning warning(Kind kind, const Repository &repository, Failure why, const QString &library = {}) {
    return {.kind = kind, .repository = repository.name, .library = library, .why = std::move(why)};
}

// What stops the rest being known: a password missing, a repository that
// can't be read, or a disk that isn't there. What follows from it isn't said
// besides.
[[nodiscard]] std::optional<Warning> blocking(const Repository &repository) {
    if (!repository.passwordAt.isEmpty())
        return warning(Kind::PasswordMissing, repository,
                       Failure(Failure::Code::SnapshotPasswordMissing, {repository.shown(), repository.passwordAt}));
    if (repository.absent)
        return warning(Kind::DiskAbsent, repository, Failure(Failure::Code::SnapshotDiskAbsent, {repository.shown(), repository.location}));
    if (!repository.unreadable.isEmpty())
        return warning(Kind::Unreadable, repository,
                       Failure(Failure::Code::SnapshotsUnreadable, {repository.shown(), repository.unreadable}));
    return std::nullopt;
}

void snapshotsOf(const Repository &repository, const QList<SnapshotWarnings::Library> &libraries, const QDateTime &now,
                 const Thresholds &thresholds, QList<Warning> &warnings) {
    if (repository.snapshot.ran.isValid() && !repository.snapshot.done)
        warnings << warning(Kind::SnapshotFailed, repository,
                            Failure(Failure::Code::SnapshotFailed, {repository.shown(), repository.snapshot.said}));
    for (const SnapshotWarnings::Library &library : libraries) {
        const auto newest = repository.newest.find(library.tag);
        if (newest == repository.newest.end() || !newest->second.isValid()) {
            if (olderThan(library.seen, thresholds.snapshotAge, now))
                warnings << warning(Kind::NoSnapshot, repository, Failure(Failure::Code::NoSnapshotYet, {library.name, repository.shown()}),
                                    library.tag);
            continue;
        }
        if (olderThan(newest->second, thresholds.snapshotAge, now))
            warnings << warning(
                Kind::SnapshotOld, repository,
                Failure(Failure::Code::SnapshotTooOld, {library.name, repository.shown()}, newest->second.secsTo(now) / 3600), library.tag);
    }
}

struct Checked {
    const SnapshotWarnings::Outcome &last;
    const QDateTime &through;
    std::chrono::days age;
    Kind failed, overdue;
    Failure::Code failedCode, overdueCode;
};

void checkOf(const Repository &repository, const Checked &checked, const QDateTime &now, QList<Warning> &warnings) {
    if (checked.last.ran.isValid() && !checked.last.done) {
        warnings << warning(checked.failed, repository, Failure(checked.failedCode, {repository.shown(), checked.last.said}));
        return;
    }
    const QDateTime since = checked.through.isValid() ? checked.through : repository.known;
    if (olderThan(since, checked.age, now))
        warnings << warning(checked.overdue, repository, Failure(checked.overdueCode, {repository.shown()}, since.secsTo(now) / 86400));
}

void roomOf(const Repository &repository, const Thresholds &thresholds, QList<Warning> &warnings) {
    if (repository.free < 0 || repository.total <= 0)
        return;
    const auto least = std::max(thresholds.fullFloor, static_cast<qint64>(static_cast<double>(repository.total) * thresholds.fullBelow));
    if (repository.free < least)
        warnings << warning(Kind::DiskNearlyFull, repository,
                            Failure(Failure::Code::SnapshotDiskNearlyFull,
                                    {repository.shown(), QLocale(QLocale::English).formattedDataSize(repository.free)}));
}

// The kinds after which a repository no longer counts as a copy.
constexpr std::array uncounted{Kind::PasswordMissing, Kind::Unreadable, Kind::DiskAbsent,  Kind::SnapshotFailed,
                               Kind::SnapshotOld,     Kind::NoSnapshot, Kind::CheckFailed, Kind::DataCheckFailed};

// Three copies on two kinds of medium, one away from home (ADR 0014): asked
// only once there are destinations, as they are what make it hold.
void threeTwoOne(const QList<Repository> &repositories, const QList<Warning> &warnings, QList<Warning> &out) {
    if (std::ranges::none_of(repositories, [](const Repository &one) { return one.medium != SnapshotWarnings::Medium::Own; }))
        return;
    const auto holds = [&](SnapshotWarnings::Medium medium) {
        return std::ranges::any_of(repositories, [&](const Repository &one) {
            return one.medium == medium && std::ranges::none_of(warnings, [&](const Warning &said) {
                       return said.repository == one.name && std::ranges::contains(uncounted, said.kind);
                   });
        });
    };
    const Repository whole{};
    if (!holds(SnapshotWarnings::Medium::Disk))
        out << warning(Kind::NoSecondDisk, whole, Failure(Failure::Code::NoSecondDiskCopy));
    if (!holds(SnapshotWarnings::Medium::Away))
        out << warning(Kind::NoCopyAway, whole, Failure(Failure::Code::NoCopyAway));
}

} // namespace

QString SnapshotWarnings::Warning::key() const { return kindName(kind) + u':' + repository + u':' + library; }

QString SnapshotWarnings::kindName(Kind kind) {
    static constexpr std::array names{"password-missing", "unreadable",     "disk-absent",   "snapshot-failed",   "snapshot-old",
                                      "no-snapshot",      "check-failed",   "check-overdue", "data-check-failed", "data-check-overdue",
                                      "disk-nearly-full", "no-second-disk", "no-copy-away"};
    return QString::fromLatin1(names.at(static_cast<std::size_t>(kind)));
}

QString SnapshotWarnings::mediumName(Medium medium) {
    static constexpr std::array names{"own", "disk", "away"};
    return QString::fromLatin1(names.at(static_cast<std::size_t>(medium)));
}

QList<Warning> SnapshotWarnings::of(const QList<Repository> &repositories, const QList<Library> &libraries, const QDateTime &now,
                                    const Thresholds &thresholds) {
    QList<Warning> warnings;
    for (const Repository &repository : repositories) {
        if (const auto stopped = blocking(repository)) {
            warnings << *stopped;
            continue;
        }
        snapshotsOf(repository, libraries, now, thresholds, warnings);
        checkOf(repository,
                {.last = repository.check,
                 .through = repository.checked,
                 .age = thresholds.checkAge,
                 .failed = Kind::CheckFailed,
                 .overdue = Kind::CheckOverdue,
                 .failedCode = Failure::Code::SnapshotCheckFailed,
                 .overdueCode = Failure::Code::SnapshotCheckOverdue},
                now, warnings);
        checkOf(repository,
                {.last = repository.dataCheck,
                 .through = repository.dataChecked,
                 .age = thresholds.dataCheckAge,
                 .failed = Kind::DataCheckFailed,
                 .overdue = Kind::DataCheckOverdue,
                 .failedCode = Failure::Code::SnapshotDataCheckFailed,
                 .overdueCode = Failure::Code::SnapshotDataCheckOverdue},
                now, warnings);
        roomOf(repository, thresholds, warnings);
    }
    QList<Warning> whole;
    threeTwoOne(repositories, warnings, whole);
    return warnings + whole;
}

QJsonObject SnapshotWarnings::json(const Warning &warning) {
    return {{u"kind"_s, kindName(warning.kind)},
            {u"repository"_s, warning.repository},
            {u"library"_s, warning.library},
            {u"text"_s, warning.why.text()}};
}
