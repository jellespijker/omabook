// Reading statistics and a year's goal (#68): the numbers worked out from the
// reading log and the books, the time the reader counts while a book is
// open, the goal kept in the change log and taken to one's other computers,
// the command and the tools, and the count of a library ten times the size
// of a real one.

#include <QDate>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <thread>

#include "fixtures.h"
#include "graph.h"
#include "library.h"
#include "mcpharness.h"
#include "readinggoal.h"
#include "readinglog.h"
#include "readingstats.h"
#include "remote.h"
#include "sync.h"

using namespace std::chrono_literals;

namespace {

[[nodiscard]] Book bookOf(qint64 id, const QString &title, double percent = 0, const QString &shelf = {}, const QString &finished = {}) {
    Book book;
    book.id = id;
    book.meta.title = title;
    book.percent = percent;
    book.shelf = shelf;
    book.finished = finished;
    return book;
}

[[nodiscard]] ReadingDay dayOf(qint64 book, const QString &day, double from, double to, qint64 seconds) {
    return {.bookId = book, .day = day, .from = from, .to = to, .seconds = seconds};
}

[[nodiscard]] const ReadingStats::BookRead *bookIn(const ReadingStats::Summary &summary, qint64 id) {
    const auto found = std::ranges::find(summary.books, id, &ReadingStats::BookRead::id);
    return found == summary.books.cend() ? nullptr : &*found;
}

// A stand-in for ssh in front of Fixtures::standInSsh: the other computer's own omabook.
QString ssh(const QTemporaryDir &dir) {
    return Fixtures::script(dir.filePath("ssh-log"), "exec '" + Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8() + "' \"$@\"\n");
}

// A clock the test moves by hand.
struct Clock {
    QDateTime now = QDateTime(QDate(2026, 3, 2), QTime(20, 0));
    [[nodiscard]] ReadingSession::Clock reading() {
        return [this] { return now; };
    }
};

} // namespace

class ReadingStatsTests : public QObject {
    Q_OBJECT

private slots:
    // Of a year: the time read, the days, how much in whole books and the
    // books finished, by month; a finished day said of a book, or else the
    // day it was read to its end; a book read without either counted apart;
    // each book's days, from the first to the finishing day.
    void aYearIsAddedUpFromTheLogAndTheBooks() {
        const QList<Book> books{
            bookOf(1, "Dune", 100, "read", "2026-02-10"), // finished on a day said
            bookOf(2, "Moss", 100),                       // read to its end, no day said: the day it got there
            bookOf(3, "Salt Roads", 40),                  // being read
            bookOf(4, "Old Friend", 0, "read"),           // read, of no known day
            bookOf(5, "Last Year", 100, "read", "2025-12-30"),
            bookOf(6, "Known Year", 0, "read", "2026"), // a year said and nothing more
        };
        const QList<ReadingDay> days{
            dayOf(1, "2026-01-05", 0, 30, 3600),   dayOf(1, "2026-02-10", 30, 100, 7200), dayOf(2, "2026-02-11", 0, 60, 1800),
            dayOf(2, "2026-02-12", 60, 100, 1800), dayOf(3, "2026-02-12", 0, 40, 600),    dayOf(5, "2025-12-30", 0, 100, 9000),
        };
        const ReadingStats::Summary summary = ReadingStats::summarize(books, days, 2026, QDate(2026, 2, 20));
        QCOMPARE(summary.total.seconds, 3600 + 7200 + 1800 + 1800 + 600);
        QCOMPARE(summary.total.days, 4); // the 12th once, though two books were read on it
        QCOMPARE(summary.total.finished, 3);
        QCOMPARE(qRound(summary.total.books * 10), 24); // 1.0 + 1.0 + 0.4
        QCOMPARE(summary.months.size(), 12);
        QCOMPARE(summary.months.at(0).tally.seconds, 3600);
        QCOMPARE(summary.months.at(0).tally.finished, 0);
        QCOMPARE(summary.months.at(1).tally.finished, 2); // Dune and Moss; Known Year has no month
        QCOMPARE(summary.months.at(1).tally.days, 3);
        QCOMPARE(summary.undated, 1);
        QCOMPARE(summary.days.size(), 4);
        QCOMPARE(summary.days.first().day, QDate(2026, 1, 5));

        const ReadingStats::BookRead *dune = bookIn(summary, 1);
        QVERIFY(dune);
        QCOMPARE(dune->begun, QDate(2026, 1, 5));
        QCOMPARE(dune->took(), 37);
        QCOMPARE(dune->seconds, 10800);
        QCOMPARE(bookIn(summary, 2)->finished, QString("2026-02-12"));
        QCOMPARE(bookIn(summary, 3)->finished, QString());
        QVERIFY(bookIn(summary, 6));  // finished in the year, if not when in it
        QVERIFY(!bookIn(summary, 5)); // of the year before
        QVERIFY(!bookIn(summary, 4));
        // The last read first.
        QCOMPARE(summary.books.first().id, 3);

        QCOMPARE(summary.years.size(), 2);
        QCOMPARE(summary.years.first().year, 2025);
        QCOMPARE(summary.years.first().tally.finished, 1);
        QCOMPARE(summary.years.first().tally.seconds, 9000);
    }

    // The days read in a row: the run still going until a day is missed
    // (yesterday counts, today not yet read), and the longest with its end.
    void daysInARowRunUntilADayIsMissed() {
        const QList<ReadingDay> days{
            dayOf(1, "2026-01-01", 0, 1, 0), dayOf(1, "2026-01-02", 1, 2, 0), dayOf(1, "2026-01-03", 2, 3, 0),
            dayOf(1, "2026-01-04", 3, 4, 0), dayOf(1, "2026-01-10", 4, 5, 0), dayOf(2, "2026-01-11", 0, 5, 0),
        };
        const QList<Book> books{bookOf(1, "One", 5), bookOf(2, "Two", 5)};
        QCOMPARE(ReadingStats::summarize(books, days, 2026, QDate(2026, 1, 12)).streaks.current, 2);
        QCOMPARE(ReadingStats::summarize(books, days, 2026, QDate(2026, 1, 11)).streaks.current, 2);
        QCOMPARE(ReadingStats::summarize(books, days, 2026, QDate(2026, 1, 13)).streaks.current, 0);
        const ReadingStats::Streaks streaks = ReadingStats::summarize(books, days, 2026, QDate(2026, 1, 13)).streaks;
        QCOMPARE(streaks.longest, 4);
        QCOMPARE(streaks.longestEnd, QDate(2026, 1, 4));
        QCOMPARE(ReadingStats::summarize(books, {}, 2026, QDate(2026, 1, 13)).streaks.longest, 0);
    }

    // The time a gap between two moves of the reader counts for: all of it,
    // up to the idle limit; nothing past it, which is someone gone away.
    void aGapCountsUntilItIsIdle() {
        QCOMPARE(ReadingLog::counted(90s), 90s);
        QCOMPARE(ReadingLog::counted(ReadingLog::idle), ReadingLog::idle);
        QCOMPARE(ReadingLog::counted(ReadingLog::idle + 1s), 0s);
        QCOMPARE(ReadingLog::counted(-5s), 0s);
    }

    // The reader's session: the time between the places it writes goes into
    // the day's reading, that of a gap too long none of it; paging back adds
    // time without widening how far it went; opening without moving is no reading.
    void theReaderCountsTheTimeABookIsReadAndLeavesTheTimeAwayOut() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("book.epub")).id;
        Clock clock;
        ReadingSession session(clock.reading());
        session.begin(10);
        clock.now = clock.now.addSecs(30);
        session.save(library, book, 10); // opened, nothing moved: no reading
        QVERIFY(Graph(library).readingLog(book).isEmpty());
        clock.now = clock.now.addSecs(90);
        session.save(library, book, 12);
        clock.now = clock.now.addSecs(120);
        session.save(library, book, 15);
        clock.now = clock.now.addSecs(3600); // went away
        session.save(library, book, 16);
        clock.now = clock.now.addSecs(60);
        session.save(library, book, 14); // paged back
        const QList<ReadingDay> days = Graph(library).readingLog(book);
        QCOMPARE(days.size(), 1);
        QCOMPARE(days.first().day, QString("2026-03-02"));
        QCOMPARE(days.first().seconds, 90 + 120 + 60);
        QCOMPARE(days.first().from, 10.0);
        QCOMPARE(days.first().to, 16.0);
        // Only opened: no day at all.
        QVERIFY(Fixtures::epub(dir.filePath("other.epub"), "Other"));
        const qint64 other = library.add(dir.filePath("other.epub")).id;
        ReadingSession again(clock.reading());
        again.begin(0);
        clock.now = clock.now.addSecs(200);
        again.save(library, other, 0);
        QVERIFY(Graph(library).readingLog(other).isEmpty());
    }

    // The goal of a year: of books or of hours, set, changed and taken away,
    // the latest said; words it doesn't know are refused.
    void aGoalIsSetChangedAndTakenAway() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(!ReadingGoal::of(library, 2026).set());
        QVERIFY(ReadingGoal::set(library, {.year = 2026, .kind = ReadingGoal::Kind::Books, .target = 24}));
        QCOMPARE(ReadingGoal::of(library, 2026).target, 24);
        QVERIFY(!ReadingGoal::of(library, 2025).set());
        QVERIFY(ReadingGoal::set(library, {.year = 2026, .kind = ReadingGoal::Kind::Hours, .target = 100}));
        QCOMPARE(ReadingGoal::of(library, 2026).kind, ReadingGoal::Kind::Hours);
        QCOMPARE(ReadingGoal::of(library, 2026).target, 100);
        QVERIFY(ReadingGoal::set(library, {.year = 2026, .kind = ReadingGoal::Kind::None, .target = 0}));
        QVERIFY(!ReadingGoal::of(library, 2026).set());
        QVERIFY(!ReadingGoal::kindNamed("pages"));
        QVERIFY(!ReadingGoal::set(library, {.year = 2026, .kind = ReadingGoal::Kind::Books, .target = -1}));
    }

    // A goal set while another connection writes the library (the window's
    // statistics are counted on a library of their own): each one is kept,
    // or said not to be, never lost without a word.
    void aGoalIsKeptWhileAnotherConnectionWrites() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("book.epub")).id;
        std::atomic<bool> stop{false};
        std::atomic<int> written{0};
        std::jthread other([&] {
            Library own(dir.filePath("Library"));
            if (!own.open())
                return;
            for (int at = 0; !stop.load(); ++at) {
                ReadingLog::add(own, book, at % 100, at % 100 + 1, 1s, QDate(2026, 1, 1).addDays(at % 300).toString(Qt::ISODate));
                ++written;
            }
        });
        QTRY_VERIFY(written.load() > 0);
        for (int target = 1; target <= 40; ++target) {
            const auto set = ReadingGoal::set(library, {.year = 2026, .kind = ReadingGoal::Kind::Books, .target = target});
            QVERIFY2(set, qPrintable(Fixtures::whyNot(set)));
            QCOMPARE(ReadingGoal::of(library, 2026).target, target);
        }
        stop = true;
    }

    // A goal set on one computer, and the time a day's reading took there,
    // are the other's after a sync; a second sync changes nothing.
    void aGoalAndTheTimeReadGoToTheOtherComputer() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "Read On Two"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("shared.epub")).id;
        const qint64 theirs = there.add(dir.filePath("shared.epub")).id;
        const Remote remote("laptop:" + dir.filePath("There"), {ssh(dir)});
        QVERIFY2(syncLibrary(here, remote).problems.isEmpty(), "the first sync");

        QVERIFY(ReadingGoal::set(here, {.year = 2026, .kind = ReadingGoal::Kind::Books, .target = 30}));
        ReadingLog::add(here, mine, 0, 20, 25min, "2026-03-02");
        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(ReadingGoal::of(there, 2026).target, 30);
        const QList<ReadingDay> days = Graph(there).readingLog(theirs);
        QCOMPARE(days.size(), 1);
        QCOMPARE(days.first().seconds, 1500);
        QCOMPARE(days.first().to, 20.0);

        // Changed there, it comes back here.
        QVERIFY(ReadingGoal::set(there, {.year = 2026, .kind = ReadingGoal::Kind::Hours, .target = 80}));
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());
        QCOMPARE(ReadingGoal::of(here, 2026).kind, ReadingGoal::Kind::Hours);
        QCOMPARE(Graph(here).readingLog(mine).first().seconds, 1500);
    }

    // `omabook stats`, its goal and one book's reading, as a person and an
    // agent meet them: the command and the tools.
    void theCommandAndTheToolsSayTheSame() {
        const McpHarness::Machine machine;
        QVERIFY(Fixtures::epub(machine.dir.filePath("book.epub"), "Counted"));
        QCOMPARE(machine.omabook({"add", machine.dir.filePath("book.epub"), "--library", machine.library}).code, 0);
        const int year = QDate::currentDate().year();
        {
            Library library(machine.library);
            QVERIFY(library.open());
            ReadingLog::add(library, 1, 0, 100, 2h, QDate::currentDate().toString(Qt::ISODate));
            library.setProgress(1, {.place = {.section = 0, .fraction = 1}, .percent = 100});
        }
        const auto set = machine.omabook({"stats", "goal", "12", "books", "--library", machine.library});
        QCOMPARE(set.code, 0);
        QVERIFY2(set.output.contains("1 of 12 books"), qPrintable(set.output + set.errors));
        const auto shown = machine.omabook({"stats", "--json", "--library", machine.library});
        QCOMPARE(shown.code, 0);
        const QJsonObject json = QJsonDocument::fromJson(shown.output.toUtf8()).object();
        QCOMPARE(json.value("year").toInt(), year);
        QCOMPARE(json.value("total").toObject().value("seconds").toInteger(), 7200);
        QCOMPARE(json.value("total").toObject().value("finished").toInt(), 1);
        QCOMPARE(json.value("goal").toObject().value("target").toInt(), 12);
        QCOMPARE(json.value("streaks").toObject().value("current").toInt(), 1);
        const auto text = machine.omabook({"stats", "--library", machine.library});
        QVERIFY2(text.output.contains("2 h 00 min read on 1 day, 1 book finished"), qPrintable(text.output));
        const auto book = machine.omabook({"stats", "--book", "1", "--json", "--library", machine.library});
        QCOMPARE(QJsonDocument::fromJson(book.output.toUtf8()).object().value("took_days").toInt(), 1);
        QVERIFY(machine.omabook({"stats", "goal", "12", "pages", "--library", machine.library}).code != 0);
        QVERIFY(machine.omabook({"stats", "--collection", "Nowhere", "--library", machine.library}).code != 0);

        McpHarness::Server server(machine);
        const QJsonObject read = server.data("reading_stats");
        QCOMPARE(read.value("total").toObject().value("seconds").toInteger(), 7200);
        QVERIFY(server.data("reading_goal_set", {{"target", 50}, {"kind", "hours"}}).value("goal").toObject().value("kind") == "hours");
        QCOMPARE(server.data("reading_stats", {{"book", 1}}).value("seconds").toInteger(), 7200);
        QVERIFY(server.data("reading_goal_set", {{"target", 0}}).value("goal").isNull());
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    // A library ten times the size of the one analysed (1,511 books), read
    // for years, is counted well within a second: a single read of the log
    // and of the books, nothing per book or per day asked of the library.
    void aLargeLibraryIsCountedQuickly() {
        constexpr int books = 15110;
        QTemporaryDir dir;
        {
            Library made(dir.filePath("Library"));
            QVERIFY(made.open());
        }
        const QString connection = "fill-" + QUuid::createUuid().toString(QUuid::Id128);
        qint64 rows = 0;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(dir.filePath("Library/omabook.db"));
            QVERIFY(database.open());
            QVERIFY(database.transaction());
            QSqlQuery book(database);
            book.prepare("INSERT INTO books (id, title, authors, path, added, shelf, finished) VALUES (?, ?, 'Someone', ?, 0, ?, ?)");
            QSqlQuery day(database);
            day.prepare("INSERT INTO reading_log (book_id, day, from_percent, to_percent, seconds) VALUES (?, ?, ?, ?, ?)");
            for (int id = 1; id <= books; ++id) {
                const bool read = id % 3 == 0;
                const QDate start = QDate(2016, 1, 1).addDays((id * 37) % 3650);
                book.addBindValue(id);
                book.addBindValue(QString("Book %1").arg(id));
                book.addBindValue(QString("Someone/Book %1 (%2)").arg(id).arg(id));
                book.addBindValue(read ? "read" : "");
                book.addBindValue(read ? start.addDays(20).toString(Qt::ISODate) : QStringLiteral("")); // NOT NULL: empty, not null
                QVERIFY(book.exec());
                // A third of the books read, each on twenty days.
                for (int at = 0; read && at < 20; ++at) {
                    day.addBindValue(id);
                    day.addBindValue(start.addDays(at).toString(Qt::ISODate));
                    day.addBindValue(at * 5.0);
                    day.addBindValue(at * 5.0 + 5);
                    day.addBindValue(1200);
                    QVERIFY(day.exec());
                    ++rows;
                }
            }
            QVERIFY(database.commit());
        }
        QSqlDatabase::removeDatabase(connection);
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QElapsedTimer timer;
        timer.start();
        const auto summary = ReadingStats::of(library, 2020, {}, QDate(2026, 11, 20));
        const qint64 took = timer.elapsed();
        QVERIFY(summary);
        qInfo().noquote() << QString("%1 books, %2 days of reading: counted in %3 ms").arg(books).arg(rows).arg(took);
        QVERIFY(summary->total.finished > 0 && summary->years.size() >= 10);
        QVERIFY2(took < 1500, qPrintable(QString("counting took %1 ms").arg(took)));
    }
};

QTEST_GUILESS_MAIN(ReadingStatsTests)
#include "readingstats_tests.moc"
