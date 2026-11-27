#include "readingcommands.h"

#include <QCommandLineParser>
#include <QDate>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <tuple>

#include "bookops.h"
#include "graph.h"
#include "library.h"
#include "librarybatch.h"
#include "numberoptions.h"
#include "readinggoal.h"
#include "readingstats.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

void printJson(const QJsonObject &object) {
    printLine(stdout, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
}

// A command's parser with the options every command has.
void common(QCommandLineParser &parser, const QString &description) {
    parser.setApplicationDescription(description);
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library",
                      "Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use).",
                      "folder"});
}

// Reads the arguments after the command's name; false, with the help printed, for a wrong number of them.
[[nodiscard]] bool parsed(QCommandLineParser &parser, const QStringList &arguments, const QString &name, qsizetype atLeast,
                          qsizetype atMost) {
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook "_s + name;
    parser.process(rest);
    const qsizetype given = parser.positionalArguments().size();
    if (given < atLeast || given > atMost) {
        printLine(stderr, parser.helpText().trimmed());
        return false;
    }
    return true;
}

[[nodiscard]] std::expected<std::unique_ptr<Library>, QString> openLibrary(const QCommandLineParser &parser) {
    auto library = std::make_unique<Library>(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    library->onBringingUpToDate([](const QString &said) { printLine(stderr, said); });
    QString error;
    if (!library->open(&error))
        return std::unexpected(error);
    return library;
}

[[nodiscard]] QString counted(qsizetype count, const QString &one, const QString &many) {
    return u"%1 %2"_s.arg(count).arg(count == 1 ? one : many);
}

[[nodiscard]] QString goalLine(const ReadingStats::Summary &summary) {
    if (!summary.goal.set())
        return u"No goal for %1 (omabook stats goal 24 books, or 100 hours)"_s.arg(summary.year);
    const bool books = summary.goal.kind == ReadingGoal::Kind::Books;
    const QString done = books ? QString::number(summary.total.finished) : hoursAndMinutes(summary.total.seconds);
    return u"Goal for %1: %2 of %3"_s.arg(summary.year)
        .arg(done, books ? counted(summary.goal.target, u"book"_s, u"books"_s) : counted(summary.goal.target, u"hour"_s, u"hours"_s));
}

void printSummary(const ReadingStats::Summary &summary) {
    const ReadingStats::Tally &total = summary.total;
    printLine(stdout, u"%1: %2 read on %3, %4 finished, %5 books' worth read"_s.arg(summary.year)
                          .arg(hoursAndMinutes(total.seconds), counted(total.days, u"day"_s, u"days"_s),
                               counted(total.finished, u"book"_s, u"books"_s))
                          .arg(total.books, 0, 'f', 1));
    printLine(stdout, goalLine(summary));
    printLine(stdout, u"Days in a row: %1 now, %2 at most"_s.arg(summary.streaks.current).arg(summary.streaks.longest)
                          + (summary.streaks.longestEnd.isValid() ? u" (to %1)"_s.arg(summary.streaks.longestEnd.toString(Qt::ISODate))
                                                                  : QString()));
    printLine(stdout, u"\nMonth\tTime\tDays\tFinished"_s);
    for (const ReadingStats::Month &month : summary.months)
        printLine(stdout,
                  u"%1\t%2\t%3\t%4"_s.arg(QDate(summary.year, month.month, 1).toString(u"MMM"_s), hoursAndMinutes(month.tally.seconds))
                      .arg(month.tally.days)
                      .arg(month.tally.finished));
    if (!summary.books.isEmpty())
        printLine(stdout, u"\nBooks"_s);
    for (const ReadingStats::BookRead &book : summary.books) {
        QString line = u"%1\t%2\t"_s.arg(book.id).arg(book.title);
        if (book.begun.isValid())
            line += u"begun %1, "_s.arg(book.begun.toString(Qt::ISODate));
        line += book.finished.isEmpty() ? u"%1%"_s.arg(qRound(book.percent)) : u"finished %1"_s.arg(book.finished);
        if (book.took() > 0)
            line += u" (%1)"_s.arg(counted(book.took(), u"day"_s, u"days"_s));
        printLine(stdout, line + u", "_s + hoursAndMinutes(book.seconds));
    }
    if (summary.undated > 0)
        printLine(stdout, u"\n%1 read without the day it was finished"_s.arg(counted(summary.undated, u"book"_s, u"books"_s)));
    if (summary.years.size() > 1) {
        printLine(stdout, u"\nYear\tTime\tDays\tFinished"_s);
        for (const ReadingStats::Year &year : summary.years)
            printLine(
                stdout,
                u"%1\t%2\t%3\t%4"_s.arg(year.year).arg(hoursAndMinutes(year.tally.seconds)).arg(year.tally.days).arg(year.tally.finished));
    }
}

// `stats goal [N books|hours | none]`: shows the year's goal, or sets it.
int goal(const Library &library, const QStringList &words, int year, bool json) {
    if (words.size() > 3)
        return failLine(u"Say a goal as a number and books or hours: omabook stats goal 24 books"_s);
    if (words.size() >= 2) {
        bool number = false;
        const int target = words.at(1).toInt(&number);
        const bool none = words.at(1).compare("none"_L1, Qt::CaseInsensitive) == 0;
        if (!number && !none)
            return failLine(u"A goal is a number of books or hours, or none: not \"%1\"."_s.arg(words.at(1)));
        const auto kind =
            none ? std::expected<ReadingGoal::Kind, QString>(ReadingGoal::Kind::None) : ReadingGoal::kindNamed(words.value(2, u"books"_s));
        if (!kind)
            return failLine(kind.error());
        if (const auto set = ReadingGoal::set(library, {.year = year, .kind = *kind, .target = none ? 0 : target}); !set)
            return failLine(set.error());
    }
    const auto summary = ReadingStats::of(library, year);
    if (!summary)
        return failLine(summary.error());
    if (json)
        printJson({{"year"_L1, year}, {"goal"_L1, ReadingStats::toJson(*summary).value("goal"_L1)}});
    else
        printLine(stdout, goalLine(*summary));
    return 0;
}

// `stats --book <id>`: one book's reading, of all time.
int showBook(const Library &library, const QString &number, bool json) {
    const auto book = ReadingStats::ofBook(library, number.toLongLong());
    if (!book)
        return failLine(u"There is no book %1 in the library."_s.arg(number));
    if (json)
        printJson(ReadingStats::toJson(*book));
    else
        printLine(stdout, u"%1\nbegun %2, last read %3, %4\n%5 on %6"_s.arg(
                              book->title, book->begun.toString(Qt::ISODate), book->last.toString(Qt::ISODate),
                              book->finished.isEmpty() ? u"%1% read"_s.arg(qRound(book->percent)) : u"finished %1"_s.arg(book->finished),
                              hoursAndMinutes(book->seconds), counted(book->days, u"day"_s, u"days"_s)));
    return 0;
}

// Why the place the options give can't be kept; none when it can (#230).
std::optional<QString> placeRefused(const Library &library, qint64 id, const QCommandLineParser &parser) {
    if (const auto why = Numbers::problem(parser, {Numbers::section(),
                                                   Numbers::fraction(),
                                                   {.name = u"percent"_s, .most = 100, .isWhole = false},
                                                   {.name = u"updated"_s, .most = static_cast<double>(Numbers::anyTime)}}))
        return why;
    // A place carried over from another computer (--updated) is kept as it was there.
    if (!parser.isSet("section") || parser.isSet("updated"))
        return std::nullopt;
    const auto fits = BookOps::checkSection(library, id, parser.value("section").toInt());
    return fits ? std::nullopt : std::optional(fits.error().text());
}

} // namespace

QString hoursAndMinutes(qint64 seconds) {
    const qint64 minutes = (seconds + 30) / 60;
    return minutes >= 60 ? u"%1 h %2 min"_s.arg(minutes / 60).arg(minutes % 60, 2, 10, QChar(u'0')) : u"%1 min"_s.arg(minutes);
}

int runProgressCli(const QStringList &arguments) {
    QCommandLineParser parser;
    common(parser, u"Show how far a book has been read, or set it."_s);
    parser.addPositionalArgument("id", "The book's number.");
    parser.addOption({"section", "The section being read, counted from 0.", "number"});
    parser.addOption({"fraction", "How far into that section, from 0 to 1.", "number"});
    parser.addOption({"percent", "How far into the book, from 0 to 100.", "number"});
    parser.addOption({"updated", "When it was read to here, in seconds since 1970; now when not given.", "seconds"});
    if (!parsed(parser, arguments, u"progress"_s, 1, 1))
        return 1;
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const QString number = parser.positionalArguments().first();
    const qint64 id = number.toLongLong();
    if (!(*library)->book(id))
        return failLine("There is no book " + number + " in the library.");
    if (const auto why = placeRefused(**library, id, parser))
        return failLine(*why);
    if (parser.isSet("section") || parser.isSet("fraction") || parser.isSet("percent")) {
        Progress read = (*library)->progress(id);
        const double before = read.percent;
        read.updated = parser.value("updated").toLongLong();
        if (parser.isSet("section"))
            read.place.section = parser.value("section").toInt();
        if (parser.isSet("fraction"))
            read.place.fraction = parser.value("fraction").toDouble();
        if (parser.isSet("percent"))
            read.percent = parser.value("percent").toDouble();
        if (!(*library)->setProgress(id, read))
            return failLine(notWritten((*library)->databasePath()).text());
        // Reading set by hand counts as reading: it goes in the log too.
        if (read.percent > before)
            Graph(**library).logReading(id, before, read.percent);
        Graph(**library).bookNode(id);
    }
    const Progress read = (*library)->progress(id);
    if (parser.isSet("json"))
        printJson(read.place.toJson({{"percent", read.percent}, {"updated", read.updated}}));
    else
        printLine(stdout, u"section %1, %2% of the book"_s.arg(read.place.section).arg(qRound(read.percent)));
    return 0;
}

int runStatsCli(const QStringList &arguments) {
    QCommandLineParser parser;
    common(parser, u"What you read, added up: the time read (counted while a book is open, the time away left out), the days read on and "
                   "in a row, the books finished and how much was read, by month and year, and each book's reading. All of it is worked "
                   "out here from the library; nothing is sent. `stats goal 24 books` (or `100 hours`, or `none`) sets the year's goal, "
                   "which your other computers take at their next sync."_s);
    parser.addOption({"year", "The year; this one when not given.", "year"});
    parser.addOption({"collection", "Only the books of this collection, by its name or path.", "name"});
    parser.addOption({"book", "Only this book's reading, by its number.", "id"});
    parser.addPositionalArgument("goal", "goal [N books|hours | none]: show the year's goal, or set it.", "[goal ...]");
    if (!parsed(parser, arguments, u"stats"_s, 0, 3))
        return 1;
    const QStringList words = parser.positionalArguments();
    if (!words.isEmpty() && words.first() != "goal"_L1) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    bool yearOk = true;
    const int year = parser.isSet("year") ? parser.value("year").toInt(&yearOk) : QDate::currentDate().year();
    if (!yearOk || year < 1 || year > 9999)
        return failLine(u"There is no year %1."_s.arg(parser.value("year")));
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    if (!words.isEmpty())
        return goal(**library, words, year, parser.isSet("json"));
    if (parser.isSet("book"))
        return showBook(**library, parser.value("book"), parser.isSet("json"));
    const auto summary = ReadingStats::of(**library, year, parser.value("collection"));
    if (!summary)
        return failLine(summary.error());
    if (parser.isSet("json"))
        printJson(ReadingStats::toJson(*summary));
    else
        printSummary(*summary);
    return 0;
}
