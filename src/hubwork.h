#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <atomic>
#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "failure.h"
#include "graph.h"
#include "graphbuilt.h"
#include "hublinks.h"
#include "hubschedule.h"

class HubKeeper;
class HubRequests;
class Library;

// The hub's long work for its libraries (#192, ADR 0011, 0013): the
// knowledge graph of each new book built whole, by the quick model the hub's
// own settings give (HubPlace::settings, through the registry, ADR 0008), so
// nothing goes to someone else's computer unless they allow hosted for quick
// work. Its results are graph facts in the library's change log, and reach
// the person's computers by the normal sync with the hub; a book's graph is
// placed where it was learned, so a reader still sees none of it past their
// own place. Beside them goes who built it and when (GraphBuilt, #219): the
// hub begins a job saying it is building the book's graph, so a computer
// doesn't begin it too, and ends it saying it built it, or stopped.
//
// One job at a time, a book each, keyed by the book, its file, the
// capability and the model: a job done is never done again, one that failed
// is tried again an hour on. Its model is asked over a copy of the library
// (HubKeeper::copy), on a thread of its own, while the daemon goes on
// answering; then, in the keeper's lock, after a moment snapshot (ADR 0014),
// its answers are laid into the library the way they were into the copy, all
// in one turn. So a job cancelled, failed or overtaken by a change to the
// same book leaves nothing of itself behind.
//
// Asked over the daemon's socket, and through a computer's session:
//   {"request": "work"}: what it is doing and has done (status());
//   {"request": "work-cancel"}: the job under way stopped ({"cancelled": whether there was one}).
class HubWork {
public:
    // One book's graph to build.
    struct Job {
        QString library; // the keeper's name for it
        qint64 book = 0; // its number there
        QString id;      // its global id, the same on every computer
        QString title;
        QString hash;  // of the file it is read from
        QString model; // what builds it: the provider, and its model when one is named
        // What makes two jobs the same: the book, its file, the capability and the model.
        [[nodiscard]] QString key() const;
    };

    static constexpr std::chrono::hours retry{1};
    // A job cancelled waits this long before it is taken up again.
    static constexpr std::chrono::hours rest{24};

    // Keeps when each job was done in `stateFolder`, works in a folder there,
    // and reads the hub's AI from `settingsFile`. Each book's graph is said to
    // be built by `name` (GraphBuilt), the hub as it calls itself.
    HubWork(HubKeeper &keeper, QString stateFolder, QString settingsFile, QString name = HubLinks::thisComputer());
    ~HubWork();
    HubWork(const HubWork &) = delete;
    HubWork &operator=(const HubWork &) = delete;
    HubWork(HubWork &&) = delete;
    HubWork &operator=(HubWork &&) = delete;

    // Looks through the libraries for what is due and starts the first job,
    // unless one is under way: back at once, the job beside the daemon.
    [[nodiscard]] std::expected<void, Failure> look();
    // The schedule's job: look().
    [[nodiscard]] HubSchedule::Job job();
    // Stops the job under way, keeping nothing of it; false when none was.
    bool cancel();
    [[nodiscard]] bool busy() const;

    // {"running": {library, book, title, kind: "graph", capability: "quick",
    //  model, done, total, started} while a job is under way, "waiting": how
    //  many more are due, "unavailable": the quick capability's status when
    //  the hub has no model for it, "last": [{library, book, title, outcome
    //  ("done", "skipped", "failed", "cancelled"), at, said}], the latest first}.
    [[nodiscard]] QJsonObject status() const;
    // "work" and "work-cancel".
    void addTo(HubRequests &requests);

private:
    struct Started {
        Job job;
        QString path;                     // the file it is read from
        std::optional<GraphPlace> before; // how far its graph was built when the job began
        int total = 0;                    // the requests it is expected to take
    };
    struct Result {
        QStringList answers; // the model's, in the order asked
        std::optional<Failure> failure;
        bool cancelled = false;
    };
    struct Kept {
        QString library, id, title, outcome, said;
        QDateTime at;
    };

    [[nodiscard]] std::expected<QList<Job>, Failure> due(const QString &model);
    [[nodiscard]] bool dueNow(const QString &key, const QDateTime &now) const;
    // The job made ready in its library; nothing when there is nothing to do for it.
    [[nodiscard]] std::optional<Started> prepare(const Job &job);
    [[nodiscard]] std::expected<void, Failure> start(const Started &started);
    // On the worker: the model asked over the copy.
    [[nodiscard]] Result run(const Started &started);
    // Back on the daemon's thread.
    void finish(const Started &started, const Result &result);
    [[nodiscard]] std::expected<QJsonObject, Failure> lay(const Started &started, const QStringList &answers);
    void note(const Job &job, const QString &outcome, const QString &said = {});
    // What the library says of the book's graph now: that this hub is building it, built it or stopped.
    [[nodiscard]] GraphBuilt::Mark says(GraphBuilt::State state) const;
    void read();
    void write() const;

    HubKeeper &m_keeper;
    QString m_file, m_scratch, m_settings, m_name;
    std::map<QString, Kept> m_kept;  // by the job's key
    std::unique_ptr<Library> m_copy; // the worker's while it runs
    std::atomic_bool m_stop{false};
    mutable std::mutex m_lock; // guards what the worker tells of its progress, and what status() reads
    std::optional<Started> m_running;
    QDateTime m_since;
    int m_done = 0;
    int m_waiting = 0;
    QJsonObject m_unavailable;
    QObject m_context; // the daemon's thread, which the worker hands its result back to
    std::unique_ptr<QThread> m_thread;
};
