#pragma once

#include <QColor>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <expected>

#include "readingstats.h"

class Backend;

// The reading statistics of a year as the window shows them (#68): the
// view of them, and the year's goal on the shelf. They are worked out
// beside the window, on a library of its own, when the library changes
// (a moment after, so a run of changes is one count), and the goal is
// set here.
class ReadingStatsView : public QObject {
    Q_OBJECT
    // The Backend whose library it is. Not called `backend`, so a view can
    // bind it to the window's `backend` without meaning this property.
    Q_PROPERTY(QObject *source READ backend WRITE setBackend NOTIFY backendChanged)
    // The year shown; this one unless another is chosen.
    Q_PROPERTY(int year READ year WRITE setYear NOTIFY askedChanged)
    // Only the books of this collection, by its path; the whole library when empty.
    Q_PROPERTY(QString collection READ collection WRITE setCollection NOTIFY askedChanged)
    // ReadingStats::toJson of the year, empty until the first count is done.
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY changed)
    // The year's goal of the whole library, whatever collection is shown:
    // {kind: "books"|"hours", target, done}, empty when there is none.
    Q_PROPERTY(QVariantMap goal READ goal NOTIFY changed)
    Q_PROPERTY(bool working READ working NOTIFY workingChanged)
    // Whether anything shows it: it counts only then, and once more when it is shown again.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit ReadingStatsView(QObject *parent = nullptr);
    ~ReadingStatsView() override { m_work.waitForFinished(); }
    ReadingStatsView(const ReadingStatsView &) = delete;
    ReadingStatsView &operator=(const ReadingStatsView &) = delete;
    ReadingStatsView(ReadingStatsView &&) = delete;
    ReadingStatsView &operator=(ReadingStatsView &&) = delete;

    QObject *backend() const;
    void setBackend(QObject *backend);
    int year() const noexcept { return m_year; }
    void setYear(int year);
    QString collection() const { return m_collection; }
    void setCollection(const QString &collection);
    QVariantMap summary() const { return m_summary; }
    QVariantMap goal() const { return m_goal; }
    bool working() const { return m_work.isRunning(); }
    bool active() const noexcept { return m_active; }
    void setActive(bool active);

    // Sets the year's goal: "books" or "hours", a target of 0 for none.
    Q_INVOKABLE void setGoal(const QString &kind, int target);
    // The colour a chart's marks are drawn in on `background`: `color`,
    // lighter or darker until it stands out from it as a mark should (3:1),
    // in a light theme and a dark one alike.
    Q_INVOKABLE QColor mark(const QColor &color, const QColor &background) const;
    // Counts again, a moment from now.
    Q_INVOKABLE void refresh();

signals:
    void backendChanged();
    void askedChanged();
    void changed();
    void workingChanged();
    void activeChanged();

private:
    using Outcome = std::expected<ReadingStats::Summary, QString>;
    void count();
    void done();

    Backend *m_backend = nullptr;
    int m_year;
    QString m_collection;
    QVariantMap m_summary, m_goal;
    QTimer m_soon;
    QFutureWatcher<Outcome> m_work;
    // Asked again while a count was under way: counted again once it is done.
    bool m_again = false;
    bool m_active = true;
    bool m_stale = false;
};
