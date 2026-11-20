#include "readinglog.h"

#include <QDate>
#include <QtGlobal>
#include <algorithm>
#include <utility>

#include "changelog.h"
#include "library.h"

using namespace Qt::StringLiterals;

std::chrono::seconds ReadingLog::counted(std::chrono::seconds gap) noexcept {
    return gap > idle || gap.count() < 0 ? std::chrono::seconds{0} : gap;
}

void ReadingLog::add(const Library &library, qint64 book, double from, double to, std::chrono::seconds time, const QString &day) {
    const bool went = to > from;
    if (book <= 0 || (!went && time.count() <= 0))
        return;
    const QString date = day.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : day;
    // One row a day: from where the day began to the furthest it got, and
    // all the time it took. A day that only paged back is where it is now.
    const double start = went ? from : to;
    ChangeLog::exec(library,
                    went ? u"INSERT INTO reading_log (book_id, day, from_percent, to_percent, seconds) VALUES (?, ?, ?, ?, ?) "
                           "ON CONFLICT (book_id, day) DO UPDATE SET from_percent = MIN(from_percent, excluded.from_percent), "
                           "to_percent = MAX(to_percent, excluded.to_percent), seconds = seconds + excluded.seconds"_s
                         : u"INSERT INTO reading_log (book_id, day, from_percent, to_percent, seconds) VALUES (?, ?, ?, ?, ?) "
                           "ON CONFLICT (book_id, day) DO UPDATE SET seconds = seconds + excluded.seconds"_s,
                    {book, date, start, to, static_cast<qint64>(std::max<qint64>(0, time.count()))});
}

QStringList ReadingLog::timedStep() {
    QStringList steps{
        u"DROP TABLE IF EXISTS reading_log_next"_s,
        u"CREATE TABLE reading_log_next (book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, day TEXT NOT NULL, "
        "from_percent REAL NOT NULL, to_percent REAL NOT NULL, seconds INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (book_id, day))"_s,
        u"INSERT INTO reading_log_next (book_id, day, from_percent, to_percent) SELECT book_id, day, from_percent, to_percent "
        "FROM reading_log"_s,
        u"DROP TABLE reading_log"_s, u"ALTER TABLE reading_log_next RENAME TO reading_log"_s};
    // Its triggers, gone with the table it was: what the change log and the
    // window's counters follow it by.
    for (const auto &[when, row] : {std::pair{"insert", "NEW"}, std::pair{"update", "NEW"}, std::pair{"delete", "OLD"}}) {
        const QString upper = QString::fromLatin1(when).toUpper();
        steps << u"CREATE TRIGGER IF NOT EXISTS reading_log_%1_dirty AFTER %2 ON reading_log BEGIN INSERT OR IGNORE INTO log_dirty "
                 "VALUES ('reading_log', %3.book_id); END"_s.arg(QLatin1StringView(when), upper, QLatin1StringView(row))
              << u"CREATE TRIGGER IF NOT EXISTS reading_log_%1_counted AFTER %2 ON reading_log BEGIN UPDATE library_state SET value = "
                 "value + 1 WHERE key = 'graph'; END"_s.arg(QLatin1StringView(when), upper);
    }
    return steps;
}

void ReadingSession::begin(double percent) {
    m_logged = percent;
    m_last = m_now();
}

void ReadingSession::save(const Library &library, qint64 book, double percent) {
    const QDateTime now = m_now();
    const std::chrono::seconds gap{m_last.isValid() ? m_last.secsTo(now) : 0};
    m_last = now;
    // Opening a book without moving is no reading, nor is the time it then
    // lies open: the time counted is that between one move and the next.
    if (qFuzzyCompare(percent + 1, m_logged + 1))
        return;
    ReadingLog::add(library, book, m_logged, percent, ReadingLog::counted(gap), now.date().toString(Qt::ISODate));
    m_logged = percent;
}
