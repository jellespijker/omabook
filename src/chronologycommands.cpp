#include "chronologycommands.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <tuple>

#include "chronologyexport.h"
#include "chronologyops.h"
#include "chronologyprint.h"
#include "graph.h"
#include "graphtools.h"
#include "library.h"
#include "settings.h"

namespace {

using namespace Qt::StringLiterals;
using Answer = std::expected<QJsonObject, QString>;

constexpr auto usage = "Usage: omabook chronology [book] [options]\n\n"
                       "The events of the books read in the order they happen in their world, not the order they are read in: of every\n"
                       "timeline (a book's, a series', a world's, real history) its events in order, with their times as the books say\n"
                       "them, what is lined up with what, the events not placed yet, and what contradicts itself, with where each was\n"
                       "said. It is cut where you are in each book.\n\n"
                       "  chronology [book] [--series s|--collection c] [--timeline t] [--from time] [--to time] [--calendar c]\n"
                       "             [--upto section [--fraction f]|--whole]\n"
                       "                                             Read it: of a book, a series, a collection, or every book\n"
                       "  chronology ... --format json|csv|graphml|dot [--output file]\n"
                       "                                             As a file other tools read: csv one row per placed event,\n"
                       "                                             graphml and dot with the order as edges\n"
                       "  chronology set-when <event> <time> [--calendar c] [--approximate] [--said words]\n"
                       "                                             When an event happens: 1944-06-06, a span 1939..1945, or JSON\n"
                       "  chronology order <event> before|after|during|same <other>\n"
                       "                                             One event in order with another\n"
                       "  chronology place <event> on <timeline>     Put an event on a timeline\n"
                       "  chronology timeline add <name> [--kind book|series|world|history|reader]\n"
                       "                                             A timeline of its own\n\n"
                       "An event or a timeline is its number or its name. What is put in by hand wins over what the books say, and is\n"
                       "marked so. Times are Y, Y-MM, Y-MM-DD, Y-MM-DDTHH or Y-MM-DDTHH:MM, on the Gregorian calendar unless --calendar\n"
                       "names the story's own. `omabook graph chronology` is the same command. Every command takes --json and\n"
                       "--library <folder>.";

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

[[nodiscard]] std::expected<std::unique_ptr<Library>, QString> openLibrary(const QCommandLineParser &parser) {
    auto library = std::make_unique<Library>(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    library->onBringingUpToDate([](const QString &said) { printLine(stderr, said); });
    QString error;
    if (!library->open(&error))
        return std::unexpected(error);
    return library;
}

void addOptions(QCommandLineParser &parser) {
    parser.setApplicationDescription(QString::fromLatin1(usage));
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library",
                      "Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use).",
                      "folder"});
    parser.addOption({"series", "Only the events of this series' books.", "series"});
    parser.addOption({"collection", "Only the events of this collection's books (omabook collections lists them).", "name"});
    parser.addOption({"scope", "A graph of its own, such as world:42; the general graph when not given.", "scope"});
    parser.addOption({"timeline", "Only this timeline, by its number or name.", "timeline"});
    parser.addOption({"from", "Only what happens from this time on.", "time"});
    parser.addOption({"to", "Only what happens up to this time.", "time"});
    parser.addOption(
        {"calendar", "The calendar of --from and --to, or of set-when's time; each timeline's own, or the Gregorian.", "calendar"});
    parser.addOption({"upto", "With a book: as known up to and including this section of it, counted from 0.", "section"});
    parser.addOption({"fraction", "With --upto: how far into that section, from 0 to 1.", "number"});
    parser.addOption({"whole", "Everything that is known, read or not: it gives away what happens."});
    parser.addOption({"approximate", "set-when: the book says around, or some time in."});
    parser.addOption({"said", "set-when: the book's own few words for the time.", "words"});
    parser.addOption({"format", "Write it as a file other tools read: json, csv, graphml or dot.", "format"});
    parser.addOption(
        {"output", "With --format: write to this file instead of printing; its suffix names the format when none is.", "file"});
    parser.addOption({"kind", "timeline add: whose it is: book, series, world, history or reader (the default).", "kind"});
}

// What is asked to be read, from the options.
[[nodiscard]] std::expected<ChronologyOps::Ask, QString> askOf(const QCommandLineParser &parser, const QString &book) {
    ChronologyOps::Ask ask;
    if (!book.isEmpty()) {
        bool number = false;
        ask.book = book.toLongLong(&number);
        if (!number || ask.book <= 0)
            return std::unexpected(u"There is no book %1: name it by its number from omabook list."_s.arg(book));
    }
    ask.series = parser.value("series");
    ask.collection = parser.value("collection");
    ask.scope = parser.value("scope");
    ask.timeline = parser.value("timeline");
    ask.from = parser.value("from");
    ask.to = parser.value("to");
    ask.calendar = parser.value("calendar");
    ask.whole = parser.isSet("whole");
    if (parser.isSet("upto")) {
        ask.upTo = GraphOps::place(ask.book, parser.value("upto"), parser.value("fraction"));
        if (!ask.upTo)
            return std::unexpected(u"--upto takes a section, counted from 0."_s);
    }
    return ask;
}

// One of the writers, as the words ask; none when they ask for none of them.
[[nodiscard]] std::optional<Answer> written(const Library &library, const QStringList &words, const QCommandLineParser &parser) {
    Graph graph(library);
    const QString sub = words.value(0);
    const QString scope = parser.value("scope");
    if (sub == "set-when"_L1 && (words.size() == 3 || (words.size() == 2 && parser.isSet("said"))))
        return ChronologyOps::setWhen(graph, words.at(1),
                                      {.time = words.value(2),
                                       .calendar = parser.value("calendar"),
                                       .words = parser.value("said"),
                                       .approximate = parser.isSet("approximate")},
                                      scope);
    if (sub == "order"_L1 && words.size() >= 4)
        return ChronologyOps::order(graph, words.at(1), words.mid(2, words.size() - 3).join(u' '), words.last(), scope);
    if (sub == "place"_L1 && words.size() == 4 && words.at(2) == "on"_L1)
        return ChronologyOps::place(graph, words.at(1), words.at(3), scope);
    if (sub == "timeline"_L1 && words.value(1) == "add"_L1 && words.size() >= 3)
        return ChronologyOps::addTimeline(graph, words.mid(2).join(u' '), parser.value("kind"));
    return std::nullopt;
}

// The chronology as a file, printed or written where --output says.
int exportTo(const QJsonObject &answer, const QCommandLineParser &parser) {
    const QString output = parser.value("output");
    QString format = parser.value("format").toLower();
    if (format.isEmpty())
        format =
            ChronologyExport::formats().contains(QFileInfo(output).suffix().toLower()) ? QFileInfo(output).suffix().toLower() : u"json"_s;
    const auto bytes = ChronologyExport::exported(answer, format);
    if (!bytes)
        return failLine(bytes.error());
    if (output.isEmpty()) {
        printLine(stdout, QString::fromUtf8(*bytes).trimmed());
        return 0;
    }
    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly) || file.write(*bytes) != bytes->size() || !file.commit())
        return failLine(u"The chronology could not be written to %1."_s.arg(output));
    if (parser.isSet("json"))
        printLine(stdout,
                  QString::fromUtf8(QJsonDocument(QJsonObject{{"format", format}, {"output", output}, {"events", answer.value("events")}})
                                        .toJson(QJsonDocument::Indented))
                      .trimmed());
    else
        printLine(stdout, u"Wrote the chronology to %1, as %2."_s.arg(output, format));
    return 0;
}

} // namespace

int runGraphCliWithChronology(const QStringList &arguments) {
    // Past the options given before the graph's command: those commandFirst moved there, and a scope.
    qsizetype at = 2;
    while (at < arguments.size() && arguments.at(at).startsWith(u'-'))
        at += arguments.at(at) == "--library"_L1 || arguments.at(at) == "--scope"_L1 ? 2 : 1;
    if (arguments.value(at) != "chronology"_L1)
        return runGraphCli(arguments);
    return runChronologyCli(arguments.first(1) + QStringList{u"chronology"_s} + arguments.mid(2, at - 2) + arguments.mid(at + 1));
}

int runChronologyCli(const QStringList &arguments) {
    QCommandLineParser parser;
    addOptions(parser);
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = "omabook chronology";
    parser.process(rest);
    const QStringList words = parser.positionalArguments();
    static const QStringList writers{"set-when", "order", "place", "timeline"};
    const bool writing = writers.contains(words.value(0));
    if (!writing && words.size() > 1)
        return failLine(QString::fromLatin1(usage));
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    Answer answer;
    if (writing) {
        const auto made = written(**library, words, parser);
        if (!made)
            return failLine(QString::fromLatin1(usage));
        answer = *made;
    } else if (const auto ask = askOf(parser, words.value(0)); ask) {
        answer = ChronologyOps::read(**library, *ask);
    } else {
        answer = std::unexpected(ask.error());
    }
    if (!answer)
        return failLine(answer.error());
    if (!writing && (parser.isSet("format") || parser.isSet("output")))
        return exportTo(*answer, parser);
    if (parser.isSet("json")) {
        printLine(stdout, QString::fromUtf8(QJsonDocument(*answer).toJson(QJsonDocument::Indented)).trimmed());
        return 0;
    }
    const Library &shelf = **library;
    const auto collection = [&shelf](qint64 id) {
        const auto found = shelf.collection(id);
        return found ? found->path : QString::number(id);
    };
    printLine(stdout, writing ? ChronologyPrint::written(*answer) : ChronologyPrint::text(*answer, collection));
    return 0;
}
