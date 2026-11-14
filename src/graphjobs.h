#pragma once

#include <QFuture>
#include <QObject>
#include <QVariantMap>
#include <atomic>
#include <memory>

#include "graphbatch.h"

class Backend;

// Building the knowledge graph for books after the fact, from the window:
// what it would cost is worked out first and shown, and the work itself runs
// beside the window, a book at a time, until it is done or stopped.
class GraphJobs : public QObject {
    Q_OBJECT
    // What the choice made would send: {books, requests, tokens, skipped: [{title, why}]}.
    Q_PROPERTY(QVariantMap plan READ plan NOTIFY planChanged)
    Q_PROPERTY(bool planning READ planning NOTIFY planChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // While at work: {index, total, title, requests, nodes, edges}.
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY progressChanged)
    // When it is over: {books, requests, nodes, edges, stopped, problems}.
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY busyChanged)

public:
    explicit GraphJobs(Backend *backend);
    ~GraphJobs() override;

    QVariantMap plan() const noexcept { return m_plan; }
    bool planning() const noexcept { return m_planning; }
    bool busy() const noexcept { return m_busy; }
    QVariantMap progress() const noexcept { return m_progress; }
    QVariantMap summary() const noexcept { return m_summary; }

    // Works out what it would take to build the graph of: "selected" (the
    // book with this id), "shown" (what the shelf shows now, with its search
    // and filters) or "read" (every book read or being read). `whole` takes
    // each book to its end, read or not.
    Q_INVOKABLE void consider(const QString &which, qint64 selectedId, bool whole);
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

signals:
    void planChanged();
    void busyChanged();
    void progressChanged();
    // The graph took something in: views of it should look again.
    void grown();

private:
    Backend *m_backend;
    GraphBatch::Choice m_choice;
    QVariantMap m_plan, m_progress, m_summary;
    bool m_planning = false, m_busy = false;
    int m_asked = 0; // the latest consider(), so an older answer is dropped
    std::shared_ptr<std::atomic_bool> m_stop = std::make_shared<std::atomic_bool>(false);
    QFuture<void> m_work;
};
