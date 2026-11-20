#pragma once

#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <expected>
#include <optional>
#include <span>

#include "readinggoal.h"

class Library;
struct Book;
struct ReadingDay;

// What the reading log and the books say of one's reading, added up (#68):
// the time read, how much was read, the books finished, the days in a row,
// by day, month and year, and per book when it was begun, read and
// finished. Worked out from what is already kept (the reading log, each
// book's finished day and shelf, its place), so a library read before
// these were counted has them too, but for the time, which is counted
// from 0.1.10 on (ReadingLog). Nothing of it is sent anywhere.
namespace ReadingStats {

// A stretch of time added up: a day, a month or a year.
struct Tally {
    qint64 seconds = 0;
    int days = 0;     // days read on
    double books = 0; // how much was read, in whole books: a day from 10% to 40% is 0.3
    int finished = 0; // books finished in it
};

struct Month {
    int month = 0; // 1 to 12
    Tally tally;
};

struct Year {
    int year = 0;
    Tally tally;
};

struct Day {
    QDate day;
    qint64 seconds = 0;
    double books = 0;
};

// One book's reading, of all time.
struct BookRead {
    qint64 id = 0;
    QString title, authors;
    QDate begun;      // the first day it was read on; invalid when it never was here
    QDate last;       // the last
    QString finished; // "2026-03-14", or as much as is known ("2024"); empty while not finished
    int days = 0;
    qint64 seconds = 0;
    double percent = 0; // how far it is read to now
    // Days from the first it was read on to the day it was finished, both
    // counted; 0 when either isn't known.
    [[nodiscard]] int took() const;
};

// The days read in a row: the run that reaches today (or yesterday, as a
// day isn't missed until it is over), and the longest ever with its last day.
struct Streaks {
    int current = 0;
    int longest = 0;
    QDate longestEnd;
};

// What one year's statistics are, with every year's beside them.
struct Summary {
    int year = 0;
    QDate today;
    Tally total;         // of the year
    QList<Month> months; // the twelve of the year
    QList<Day> days;     // each day of the year read on, in order
    QList<Year> years;   // every year read in or a book finished in, oldest first
    Streaks streaks;
    QList<BookRead> books;                 // read or finished in the year, the last read first
    int undated = 0;                       // books read, of any year, whose finishing day isn't known
    ReadingGoal::Goal goal;                // what the year was set to be, if anything
    [[nodiscard]] double goalDone() const; // books finished, or hours read, as the goal counts
};

// The statistics of `year` of these books' reading, as of `today`.
[[nodiscard]] Summary summarize(std::span<const Book> books, std::span<const ReadingDay> days, int year, QDate today);

// The same of a library: every book, or those of one collection (by its
// name or path), with the year's goal; why not, when there is no such collection.
[[nodiscard]] std::expected<Summary, QString> of(const Library &library, int year, const QString &collection = {},
                                                 QDate today = QDate::currentDate());

// One book's reading, from the library; nothing when there is no such book.
[[nodiscard]] std::optional<BookRead> ofBook(const Library &library, qint64 id);

// {"year", "today", "total": {seconds, days, books, finished}, "months": [...], "days": [...], "years": [...],
//  "streaks": {current, longest, longest_end}, "books": [...], "undated", "goal": {kind, target, done} | null}.
[[nodiscard]] QJsonObject toJson(const Summary &summary);
[[nodiscard]] QJsonObject toJson(const BookRead &book);

} // namespace ReadingStats
