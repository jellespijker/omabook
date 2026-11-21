#include "recognitionqueue.h"

#include <QDateTime>
#include <algorithm>
#include <utility>

#include "failure.h"
#include "log.h"
#include "recognitionstore.h"

using namespace std::chrono_literals;
using Why = Recognition::Failure::Why;

struct RecognitionQueue::Job {
    int id = 0;
    Request request;
    Done done;
    Waiting waiting;
    RecognitionRoute::Ways ways;
    std::size_t way = 0;
    QString key;         // of the way under way, once resolved
    QStringList without; // why the ways tried had no such service
    QString away;        // why a way couldn't be reached, this round
    bool lacking = false; // a way lacked the model asked for
    std::chrono::milliseconds wait{0};
};

RecognitionQueue::RecognitionQueue(Route route, LibraryOf library, QObject *parent)
    : QObject(parent), m_route(std::move(route)), m_library(std::move(library)), m_firstWait(15s), m_longestWait(5min) {
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_jobs.empty())
            return;
        Job &job = *m_jobs.front();
        job.way = 0;
        job.without.clear();
        job.away.clear();
        job.lacking = false;
        step(job);
    });
}

RecognitionQueue::~RecognitionQueue() {
    // The ways go before anything they might still tell.
    for (const auto &job : m_jobs)
        for (const auto &way : job->ways)
            way->disconnect(this);
}

RecognitionQueue::Route RecognitionQueue::routeFor(const Settings &settings) {
    return [&settings](Recognition::Kind kind, bool hosted) { return RecognitionRoute::ways(settings, kind, hosted); };
}

std::optional<QString> RecognitionQueue::lacking(Recognition::Kind kind, bool hosted) const {
    const auto found = m_lacking.find(kind);
    if (found == m_lacking.end() || found->second.signature != signatureNow(kind, hosted))
        return std::nullopt;
    return found->second.why;
}

QString RecognitionQueue::signatureNow(Recognition::Kind kind, bool hosted) const {
    // Made afresh, before any is asked: the ways a job went have learnt their models.
    const auto ways = m_route(kind, hosted);
    return ways ? RecognitionRoute::signature(*ways) : QString();
}

void RecognitionQueue::setRetry(std::chrono::milliseconds first, std::chrono::milliseconds most) {
    m_firstWait = first;
    m_longestWait = most;
}

int RecognitionQueue::submit(Request request, Done done, Waiting waiting) {
    auto job = std::make_unique<Job>();
    job->id = m_nextJob++;
    job->request = std::move(request);
    job->done = std::move(done);
    job->waiting = std::move(waiting);
    const int id = job->id;
    m_jobs.push_back(std::move(job));
    // Never before the asker has the number back.
    QTimer::singleShot(0, this, &RecognitionQueue::next);
    return id;
}

bool RecognitionQueue::cancel(int job) {
    const auto found = std::ranges::find_if(m_jobs, [job](const auto &queued) { return queued->id == job; });
    if (found == m_jobs.end())
        return false;
    if (found == m_jobs.begin() && m_running) {
        m_retry.stop();
        for (const auto &way : (*found)->ways)
            way->cancel();
        finish(std::unexpected(Failure(Failure::Code::RecognitionCancelled).text()));
        return true;
    }
    const std::unique_ptr<Job> cancelled = std::move(*found);
    m_jobs.erase(found);
    if (cancelled->done)
        cancelled->done(std::unexpected(Failure(Failure::Code::RecognitionCancelled).text()));
    return true;
}

void RecognitionQueue::next() {
    if (m_running || m_jobs.empty())
        return;
    m_running = true;
    Job &job = *m_jobs.front();
    auto ways = m_route(job.request.input.kind, job.request.hosted);
    if (!ways) {
        finish(std::unexpected(ways.error()));
        return;
    }
    job.ways = std::move(*ways);
    for (const auto &way : job.ways) {
        connect(way.get(), &Recognition::Recognizer::resolved, this, [this, &job] { resolved(job); });
        connect(way.get(), &Recognition::Recognizer::failed, this,
                [this, &job](const Recognition::Failure &failure) { failed(job, failure); });
        connect(way.get(), &Recognition::Recognizer::recognized, this, [this, &job](Recognition::Result result) {
            if (result.at == 0)
                result.at = QDateTime::currentMSecsSinceEpoch();
            if (const Library *library = m_library ? m_library() : nullptr; library != nullptr && !job.request.owner.isEmpty())
                if (const auto kept = RecognitionStore::keep(*library, job.request.owner, job.key, result); !kept)
                    qCWarning(lcAi).noquote() << "A recognised" << Recognition::kindName(result.kind)
                                              << "couldn't be kept:" << kept.error();
            finish(result);
        });
    }
    step(job);
}

void RecognitionQueue::step(Job &job) {
    if (job.way < job.ways.size()) {
        job.ways.at(job.way)->resolve();
        return;
    }
    // Every way tried. One that was away is waited for; else none can do it.
    if (!job.away.isEmpty()) {
        job.wait = job.wait == 0ms ? m_firstWait : std::min(job.wait * 2, m_longestWait);
        if (job.waiting)
            job.waiting(job.away);
        m_retry.start(job.wait);
        return;
    }
    finish(std::unexpected(job.without.join(u' ')));
}

void RecognitionQueue::resolved(Job &job) {
    Recognition::Recognizer &way = *job.ways.at(job.way);
    const auto key = Recognition::key(job.request.input, way.model());
    if (!key) {
        finish(std::unexpected(key.error()));
        return;
    }
    job.key = *key;
    const Library *library = m_library ? m_library() : nullptr;
    if (!job.request.again && library != nullptr && !job.request.owner.isEmpty())
        if (const auto kept = RecognitionStore::find(*library, job.request.owner, job.key)) {
            finish(*kept);
            return;
        }
    // What is recognised is never written down, only where it went.
    qCInfo(lcAi).noquote() << "Recognising" << Recognition::kindName(job.request.input.kind) << "with" << way.model() << "at"
                           << way.where();
    way.recognize(job.request.input);
}

void RecognitionQueue::failed(Job &job, const Recognition::Failure &failure) {
    job.lacking = job.lacking || failure.lacking;
    switch (failure.why) {
    case Why::Refused:
        finish(std::unexpected(failure.message));
        return;
    case Why::Away:
        job.away = failure.message;
        break;
    case Why::Unsupported:
        job.without << failure.message;
        break;
    }
    ++job.way;
    step(job);
}

void RecognitionQueue::finish(const std::expected<Recognition::Result, QString> &outcome) {
    if (m_jobs.empty())
        return;
    const std::unique_ptr<Job> job = std::move(m_jobs.front());
    m_jobs.pop_front();
    m_running = false;
    m_retry.stop();
    for (const auto &way : job->ways)
        way->disconnect(this);
    const Recognition::Kind kind = job->request.input.kind;
    if (outcome)
        m_lacking.erase(kind);
    else if (job->lacking)
        m_lacking.insert_or_assign(kind, Lacking{.signature = signatureNow(kind, job->request.hosted), .why = outcome.error()});
    if (job->done)
        job->done(outcome);
    // Its ways may be the ones telling this: they go once they are done telling.
    for (auto &way : job->ways)
        way.release()->deleteLater();
    QTimer::singleShot(0, this, &RecognitionQueue::next);
}
