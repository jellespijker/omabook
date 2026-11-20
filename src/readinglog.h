#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <chrono>
#include <functional>
#include <utility>

class Library;

// The time a day's reading of a book took (#68), beside how far it went
// (Graph::logReading): what the reader counts while a book is open, with
// the time someone was away left out, and what the statistics add up.
namespace ReadingLog {

// Two moves of the reader further apart than this are someone who went
// away, or put the book down: none of the time between them counts. A page
// takes a minute or two to read; a hard one, a few.
inline constexpr std::chrono::seconds idle{std::chrono::minutes{5}};

// What a gap between two moves of the reader counts for: all of it, or
// nothing when it is longer than `idle`.
[[nodiscard]] std::chrono::seconds counted(std::chrono::seconds gap) noexcept;

// Adds `time` to a day's reading of a book, and when `to` is past `from`,
// that it went from there to there; today when no day is given. Paging back
// widens nothing, but the time it took still counts.
void add(const Library &library, qint64 book, double from, double to, std::chrono::seconds time, const QString &day = {});

// The format step that gives the reading log its time (format 37): the
// table made anew with it, as SQLite adds a column only once, so the step
// can be taken again by a library put back to the format before.
[[nodiscard]] QStringList timedStep();

} // namespace ReadingLog

// One book open in the reader, as the reading log hears of it: where it
// was when that was last written, and when the reader last moved. Each time
// the place is written after a move (a moment after the reader stops), the
// time since the last is added to the day's reading, unless it was idle.
class ReadingSession {
public:
    using Clock = std::function<QDateTime()>;
    explicit ReadingSession(Clock now = [] { return QDateTime::currentDateTime(); }) : m_now(std::move(now)) {}

    // The book was opened at `percent`: nothing is read yet.
    void begin(double percent);
    // The reader is at `percent` now: the time since the last and how far it went, into the log.
    void save(const Library &library, qint64 book, double percent);

private:
    Clock m_now;
    double m_logged = 0;
    QDateTime m_last;
};
