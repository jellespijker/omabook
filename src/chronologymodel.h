#pragma once

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

#include "chronologychart.h"
#include "chronologyread.h"

class Backend;
class Library;

// The chronology (#44) as one view of it browses it: what it was opened
// from (a book, a series, a collection or a whole graph), laid out as
// lanes on shared axes (chronologychart.h), which of its events the filters
// let through, the event that is selected and what is known of it.
//
// Never ahead of the reader: everything it shows is read as far as they
// read each book (chronologyread.h), the filters only ever narrow that.
class ChronologyBrowser : public QObject {
    Q_OBJECT
    // The Backend whose library is read. Not called `backend`, so a view can
    // bind it to the window's `backend` without meaning this property.
    Q_PROPERTY(QObject *source READ backend WRITE setBackend NOTIFY backendChanged)
    // What it was opened from: "scope", "book", "series" or "collection",
    // and its name (a book's title).
    Q_PROPERTY(QString showing READ showing NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    // How many events are on a lane, and how many of them the filters let through.
    Q_PROPERTY(int eventCount READ eventCount NOTIFY changed)
    Q_PROPERTY(int shownCount READ shownCount NOTIFY changed)
    Q_PROPERTY(int laneCount READ laneCount NOTIFY changed)
    Q_PROPERTY(int contradictionCount READ contradictionCount NOTIFY changed)
    // The events of which no time or order is known, as the filters let
    // them through: [{id, name, said, from}], listed apart.
    Q_PROPERTY(QVariantList unplaced READ unplaced NOTIFY changed)
    Q_PROPERTY(bool showUnplaced READ showUnplaced WRITE setShowUnplaced NOTIFY changed)
    // The filters that are on, each a chip: [{act, value, label}].
    Q_PROPERTY(QVariantList filters READ filters NOTIFY changed)
    Q_PROPERTY(qint64 selected READ selected WRITE select NOTIFY selectedChanged)
    // What is known of the selected event: its card. Empty when none is.
    Q_PROPERTY(QVariantMap details READ details NOTIFY selectedChanged)

public:
    explicit ChronologyBrowser(QObject *parent = nullptr);

    QObject *backend() const;
    void setBackend(QObject *backend);
    // Tests work on a library of their own, without a backend.
    void setLibrary(const Library *library);

    QString showing() const;
    QString title() const { return m_title; }
    int eventCount() const { return static_cast<int>(m_chart.marksOf.size()); }
    int shownCount() const { return m_shownCount; }
    int laneCount() const { return static_cast<int>(m_chart.lanes.size()); }
    int contradictionCount() const { return static_cast<int>(m_chart.contradictions.size()); }
    QVariantList unplaced() const;
    bool showUnplaced() const noexcept { return m_showUnplaced; }
    void setShowUnplaced(bool show);
    QVariantList filters() const;
    qint64 selected() const noexcept { return m_selected; }
    QVariantMap details() const noexcept { return m_details; }

    // Where it is opened from: a whole graph ("" for the general one), a
    // book, a series, or a collection by its name. A book, a series and a
    // collection are each their books' filter, which can be taken off.
    Q_INVOKABLE void showScope(const QString &scope = {});
    Q_INVOKABLE void showBook(qint64 bookId);
    Q_INVOKABLE void showSeries(const QString &series);
    Q_INVOKABLE void showCollection(const QString &collection);
    // Reads it all again, keeping what is shown, filtered and selected.
    Q_INVOKABLE void refresh();

    // What can be filtered by, for the filter's list: [{act, value, label,
    // kindName, on, count, common}], with the filters that are on first.
    Q_INVOKABLE QVariantList choices() const;
    // A filter on or off: "timeline" (a lane, by its number), "book",
    // "series", "involved" (a character, place, group or theme), "met"
    // (only the events met in a book, not those only implied).
    Q_INVOKABLE void setFilter(const QString &act, const QVariant &value, bool on);
    // A stretch of story time, as years or dates ("1939", "1944-06"), on
    // the calendar of each band; either end may be empty. False when one
    // of them is no time.
    Q_INVOKABLE bool setRange(const QString &from, const QString &to);
    Q_INVOKABLE void clearFilters();

    Q_INVOKABLE void select(qint64 event);
    // The next event along its lane ("left", "right"), or the nearest on
    // the lane above or below ("up", "down"); the first when none is selected.
    Q_INVOKABLE void step(const QString &direction);
    // The events going by these words, the first in the order of the lanes: [{id, name}].
    Q_INVOKABLE QVariantList find(const QString &words) const;

    // For the picture.
    const Chronology::Chart::Chart &chart() const noexcept { return m_chart; }
    // Whether the filters let each mark through, by its number in chart().marks.
    const QList<bool> &passes() const noexcept { return m_passes; }
    // What it is called, and whose shared graph it is of (empty for one's own).
    QString nameOf(qint64 event) const;
    QString fromOf(qint64 event) const;
    // The name of a lane, and of a band (its calendar, or that it is by order alone).
    QString laneName(int lane) const;
    QString bandName(int band) const;
    // A tick's words: a year, a month, a day, as the calendar has it.
    static QString tickText(const QString &calendar, const EventTime::Point &point);
    // The mark of the selected event that was stepped to (it may be on
    // several lanes); -1 for none.
    int selectedMark() const noexcept { return m_mark; }
    // The event the reader has come to last, in the book they are reading: "you are here".
    qint64 here() const noexcept { return m_here; }

signals:
    void backendChanged();
    // What is shown changed: the events, the lanes, the filters.
    void changed();
    void selectedChanged();

private:
    const Library *library() const;
    void open(Chronology::Reading::From from);
    void load();
    void filter();
    void describe();
    [[nodiscard]] bool passes(const Chronology::Chart::Mark &mark) const;
    [[nodiscard]] bool passesEvent(qint64 event) const;
    [[nodiscard]] QString laneKey(int lane) const;
    // The stretch of time asked for, as its chip says it.
    [[nodiscard]] QString rangeName() const;
    [[nodiscard]] qint64 findHere() const;
    // Stepping: whether a mark is let through, and the next one along its
    // lane or across the lanes (-1 for none) from the one stepped to.
    [[nodiscard]] bool shownMark(int mark) const;
    [[nodiscard]] int along(int by) const;
    [[nodiscard]] int across(int by) const;
    void takeMark(int mark);

    Backend *m_backend = nullptr;
    const Library *m_library = nullptr;
    Chronology::Reading::From m_from;
    QString m_title;
    Chronology::Reading::Read m_read;
    Chronology::Chart::Chart m_chart;
    QList<bool> m_passes;
    int m_shownCount = 0;
    bool m_showUnplaced = true;
    // The filters: each set is "any of these"; an empty one lets all through.
    QSet<QString> m_lanes; // by laneKey()
    QSet<qint64> m_books, m_involved;
    QSet<QString> m_series;                // "series:…" and "collection:…"
    QHash<QString, QSet<qint64>> m_groups; // the books of each
    bool m_onlyMet = false;
    QString m_rangeFrom, m_rangeTo;
    qint64 m_selected = 0;
    int m_mark = -1;
    QVariantMap m_details;
    qint64 m_here = 0;
    QTimer m_refreshSoon;
};
