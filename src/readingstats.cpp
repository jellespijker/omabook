#include "readingstats.h"

#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <ranges>
#include <tuple>

#include "failure.h"
#include "graph.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

// As Book::status counts a book read by how far it was read.
constexpr double readTo = 98;

// The year and month a finished day says, as much as it says: "2026-03-14",
// "2026-03" or "2026"; 0 for what isn't said.
struct When {
    int year = 0;
    int month = 0;
};

[[nodiscard]] When whenOf(const QString &finished) {
    bool yearOk = false;
    bool monthOk = false;
    const int year = finished.left(4).toInt(&yearOk);
    const int month = finished.size() >= 7 ? finished.mid(5, 2).toInt(&monthOk) : 0;
    return {.year = yearOk ? year : 0, .month = monthOk && month >= 1 && month <= 12 ? month : 0};
}

// What is known of one book's reading as the days are gone through.
struct Reading {
    ReadingStats::BookRead read;
    QString reachedEnd; // the first day it was read to its end, for a book with no finished day said
    QSet<int> years;    // the years it was read in
};

// The day a book was finished: the one said of it, else the first it was
// read to its end on, for a book read as far as counts as read; empty
// while it isn't, or when that day isn't known (`undated`).
[[nodiscard]] QString finishedOf(const Book &book, const Reading &reading, bool &undated) {
    undated = false;
    if (!book.finished.isEmpty())
        return book.finished;
    if (book.status() != "read"_L1)
        return {};
    if (reading.reachedEnd.isEmpty())
        undated = true;
    return reading.reachedEnd;
}

// What a book itself says, with what the log said of it in `reading`.
void fill(Reading &reading, const Book &book, bool &undated) {
    reading.read.id = book.id;
    reading.read.title = book.meta.title;
    reading.read.authors = book.meta.authors.join(", "_L1);
    reading.read.percent = book.percent;
    reading.read.finished = finishedOf(book, reading, undated);
}

void addDay(ReadingStats::Tally &tally, const ReadingDay &day) {
    tally.seconds += day.seconds;
    tally.books += std::max(0.0, day.to - day.from) / 100;
}

[[nodiscard]] ReadingStats::Streaks streaksOf(const QSet<QDate> &read, QDate today) {
    QList<QDate> days(read.cbegin(), read.cend());
    std::ranges::sort(days);
    ReadingStats::Streaks streaks;
    int run = 0;
    for (qsizetype at = 0; at < days.size(); ++at) {
        run = at > 0 && days.at(at - 1).daysTo(days.at(at)) == 1 ? run + 1 : 1;
        if (run > streaks.longest) {
            streaks.longest = run;
            streaks.longestEnd = days.at(at);
        }
    }
    // A run is still going while its last day is today or yesterday.
    if (!days.isEmpty() && days.last().daysTo(today) <= 1 && days.last() <= today)
        streaks.current = run;
    return streaks;
}

// Every day and book of the log gone through once.
struct Gathered {
    QHash<qint64, Reading> books;
    QMap<int, ReadingStats::Year> years;
    QMap<QDate, ReadingStats::Day> days; // of the year
    QSet<QDate> read;                    // every day read on
    QHash<int, QSet<QDate>> yearDays;
    QHash<int, QSet<QDate>> monthDays; // of the year, by month
};

void gather(Gathered &into, std::span<const ReadingDay> days, int year) {
    for (const ReadingDay &day : days) {
        const QDate date = QDate::fromString(day.day, Qt::ISODate);
        if (!date.isValid())
            continue;
        Reading &reading = into.books[day.bookId];
        ReadingStats::BookRead &book = reading.read;
        if (!book.begun.isValid() || date < book.begun)
            book.begun = date;
        if (!book.last.isValid() || date > book.last)
            book.last = date;
        ++book.days;
        book.seconds += day.seconds;
        if (day.to >= readTo && (reading.reachedEnd.isEmpty() || day.day < reading.reachedEnd))
            reading.reachedEnd = day.day;
        reading.years.insert(date.year());
        into.read.insert(date);
        into.yearDays[date.year()].insert(date);
        ReadingStats::Year &ofYear = into.years[date.year()];
        ofYear.year = date.year();
        addDay(ofYear.tally, day);
        if (date.year() != year)
            continue;
        ReadingStats::Day &ofDay = into.days[date];
        ofDay.day = date;
        ofDay.seconds += day.seconds;
        ofDay.books += std::max(0.0, day.to - day.from) / 100;
        into.monthDays[date.month()].insert(date);
    }
}

} // namespace

int ReadingStats::BookRead::took() const {
    const QDate end = QDate::fromString(finished, Qt::ISODate);
    return begun.isValid() && end.isValid() && end >= begun ? static_cast<int>(begun.daysTo(end)) + 1 : 0;
}

double ReadingStats::Summary::goalDone() const {
    switch (goal.kind) {
    case ReadingGoal::Kind::Books:
        return total.finished;
    case ReadingGoal::Kind::Hours:
        return static_cast<double>(total.seconds) / 3600;
    case ReadingGoal::Kind::None:
        break;
    }
    return 0;
}

ReadingStats::Summary ReadingStats::summarize(std::span<const Book> books, std::span<const ReadingDay> days, int year, QDate today) {
    Summary summary{.year = year, .today = today};
    Gathered gathered;
    gather(gathered, days, year);
    for (int month = 1; month <= 12; ++month)
        summary.months << Month{.month = month};
    for (const Book &book : books) {
        Reading &reading = gathered.books[book.id];
        bool undated = false;
        fill(reading, book, undated);
        summary.undated += undated ? 1 : 0;
        const When when = whenOf(reading.read.finished);
        if (when.year > 0) {
            Year &ofYear = gathered.years[when.year];
            ofYear.year = when.year;
            ++ofYear.tally.finished;
        }
        if (when.year == year && when.month > 0)
            ++summary.months[when.month - 1].tally.finished;
        if (when.year == year || reading.years.contains(year))
            summary.books << reading.read;
    }
    for (auto it = gathered.years.begin(); it != gathered.years.end(); ++it)
        it->tally.days = static_cast<int>(gathered.yearDays.value(it.key()).size());
    summary.years = gathered.years.values();
    summary.total = gathered.years.value(year, Year{.year = year}).tally;
    for (Month &month : summary.months)
        month.tally.days = static_cast<int>(gathered.monthDays.value(month.month).size());
    for (const Day &day : std::as_const(gathered.days)) {
        Tally &month = summary.months[day.day.month() - 1].tally;
        month.seconds += day.seconds;
        month.books += day.books;
    }
    summary.days = gathered.days.values();
    summary.streaks = streaksOf(gathered.read, today);
    std::ranges::sort(summary.books, [](const BookRead &one, const BookRead &other) {
        return std::tie(one.last, one.id) > std::tie(other.last, other.id);
    });
    return summary;
}

std::expected<ReadingStats::Summary, QString> ReadingStats::of(const Library &library, int year, const QString &collection, QDate today) {
    QList<Book> books = library.books();
    if (!collection.isEmpty()) {
        const std::optional<Collection> chosen = library.collectionNamed(collection);
        if (!chosen)
            return std::unexpected(Failure(Failure::Code::NoSuchCollection, {collection}).text());
        books = chosen->of(books);
    }
    // The whole library's log, of every tenant: one's reading isn't the graph's.
    QList<ReadingDay> days = Graph(library).readingLog();
    // A day of a book no longer here is still a day read, but of no collection.
    if (!collection.isEmpty()) {
        QSet<qint64> ids;
        for (const Book &book : std::as_const(books))
            ids.insert(book.id);
        days.removeIf([&ids](const ReadingDay &day) { return !ids.contains(day.bookId); });
    }
    Summary summary = summarize(books, days, year, today);
    summary.goal = ReadingGoal::of(library, year);
    return summary;
}

std::optional<ReadingStats::BookRead> ReadingStats::ofBook(const Library &library, qint64 id) {
    const std::optional<Book> book = library.book(id);
    if (!book)
        return std::nullopt;
    const QList<ReadingDay> days = Graph(library).readingLog(id);
    Gathered gathered;
    gather(gathered, days, 0);
    Reading reading = gathered.books.value(id);
    bool undated = false;
    fill(reading, *book, undated);
    return reading.read;
}

namespace {

[[nodiscard]] QJsonObject tallyJson(const ReadingStats::Tally &tally) {
    return {{"seconds"_L1, tally.seconds},
            {"days"_L1, tally.days},
            {"books"_L1, qRound(tally.books * 100) / 100.0},
            {"finished"_L1, tally.finished}};
}

[[nodiscard]] QString dayText(QDate day) { return day.isValid() ? day.toString(Qt::ISODate) : QString(); }

} // namespace

QJsonObject ReadingStats::toJson(const BookRead &book) {
    return {{"id"_L1, book.id},
            {"title"_L1, book.title},
            {"authors"_L1, book.authors},
            {"begun"_L1, dayText(book.begun)},
            {"last"_L1, dayText(book.last)},
            {"finished"_L1, book.finished},
            {"days"_L1, book.days},
            {"seconds"_L1, book.seconds},
            {"percent"_L1, qRound(book.percent * 10) / 10.0},
            {"took_days"_L1, book.took()}};
}

QJsonObject ReadingStats::toJson(const Summary &summary) {
    QJsonArray months;
    for (const Month &month : summary.months) {
        QJsonObject object = tallyJson(month.tally);
        object.insert("month"_L1, month.month);
        months << object;
    }
    QJsonArray days;
    for (const Day &day : summary.days)
        days << QJsonObject{{"day"_L1, dayText(day.day)}, {"seconds"_L1, day.seconds}, {"books"_L1, qRound(day.books * 100) / 100.0}};
    QJsonArray years;
    for (const Year &year : summary.years) {
        QJsonObject object = tallyJson(year.tally);
        object.insert("year"_L1, year.year);
        years << object;
    }
    QJsonArray books;
    for (const BookRead &book : summary.books)
        books << toJson(book);
    const QJsonValue goal = summary.goal.set() ? QJsonValue(QJsonObject{{"kind"_L1, ReadingGoal::nameOf(summary.goal.kind)},
                                                                        {"target"_L1, summary.goal.target},
                                                                        {"done"_L1, qRound(summary.goalDone() * 10) / 10.0}})
                                               : QJsonValue(QJsonValue::Null);
    return {{"year"_L1, summary.year},
            {"today"_L1, dayText(summary.today)},
            {"total"_L1, tallyJson(summary.total)},
            {"months"_L1, months},
            {"days"_L1, days},
            {"years"_L1, years},
            {"streaks"_L1, QJsonObject{{"current"_L1, summary.streaks.current},
                                       {"longest"_L1, summary.streaks.longest},
                                       {"longest_end"_L1, dayText(summary.streaks.longestEnd)}}},
            {"books"_L1, books},
            {"undated"_L1, summary.undated},
            {"goal"_L1, goal}};
}
