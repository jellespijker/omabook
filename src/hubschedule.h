#pragma once

#include <QDateTime>
#include <QString>
#include <QTime>
#include <QTimer>
#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <optional>

#include "failure.h"

class HubKeeper;

// The hub's own work at its times (#179): the nightly snapshot, the weekly
// check and the monthly test restore (#181, #184) are jobs here. Each runs on
// the daemon's thread, between requests, and takes the libraries from the
// keeper as a request does, for as long as it needs them alone. When each last
// ran, and how, is kept in the state folder, so a job missed while the hub was
// off runs as soon as it is back, and one that failed is tried again an hour on.
class HubSchedule {
public:
    using Job = std::function<std::expected<void, Failure>(HubKeeper &keeper)>;

    // How a job went the last time it ran: what the warnings (#184) read.
    struct Outcome {
        QDateTime ran;
        bool done = false;
        QString said; // why not, in English
    };

    static constexpr std::chrono::hours retry{1};

    HubSchedule(HubKeeper &keeper, QString stateFolder);

    // `job` once a day at `at`, local time; the first time at once.
    void daily(const QString &name, QTime at, Job job);
    // `job` once every `interval`; the first time at once.
    void every(const QString &name, std::chrono::seconds interval, Job job);

    // Runs what is due, and from then on each job when it is due.
    void start();
    // Runs each job that is due at `now`, one after another.
    void runDue(const QDateTime &now);
    [[nodiscard]] std::optional<Outcome> outcome(const QString &name) const;
    [[nodiscard]] QDateTime due(const QString &name) const;

    // When a job that runs daily at `at` is due after it last ran at `last`
    // (never, when that isn't valid: then at once, at `now`).
    [[nodiscard]] static QDateTime dueDaily(const QDateTime &last, QTime at, const QDateTime &now);

private:
    struct Planned {
        Job job;
        std::optional<QTime> at;          // daily, at this time
        std::chrono::seconds interval{0}; // or every this long
    };

    [[nodiscard]] QDateTime dueOf(const QString &name, const Planned &planned, const QDateTime &now) const;
    void read();
    void write() const;
    void wake();

    HubKeeper &m_keeper;
    QString m_file;
    std::map<QString, Planned> m_jobs;
    std::map<QString, Outcome> m_outcomes;
    QTimer m_timer;
};
