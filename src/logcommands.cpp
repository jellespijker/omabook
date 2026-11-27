#include "logcommands.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cstdio>
#include <expected>
#include <print>
#include <tuple>

#include "log.h"
#include "numberoptions.h"

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

// "30m", "2h", "1d" ago, or a date and time.
[[nodiscard]] std::expected<QDateTime, QString> sinceWhen(const QString &since) {
    static const QRegularExpression ago("^(\\d+)([mhd])$");
    if (const QRegularExpressionMatch match = ago.match(since); match.hasMatch()) {
        static const QHash<QString, qint64> secondsIn{{"m", 60}, {"h", 3600}, {"d", 86400}};
        return QDateTime::currentDateTime().addSecs(-match.captured(1).toLongLong() * secondsIn.value(match.captured(2)));
    }
    if (QDateTime at = QDateTime::fromString(since, Qt::ISODate); at.isValid())
        return at;
    return std::unexpected(QStringLiteral("--since takes 30m, 2h, 1d, or a date and time like 2026-11-14T21:00."));
}

} // namespace

int runLogsCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Show what Omabook did and what went wrong: the window, every command and the MCP server write "
                                     "to one log. It holds what happened, how big and how long; never the text of a book, a question "
                                     "or an answer. The newest lines come last.");
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library",
                      "Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use).",
                      "folder"});
    parser.addOption({{"n", "lines"}, "How many of the newest lines; 100 when not given, 0 for all.", "count", "100"});
    parser.addOption({"level", "Only this level and above: debug, info, warning, critical.", "level", "debug"});
    parser.addOption({"category", "Only one part: ai, library, graph, plugins, sites, sync, app, qml.", "part"});
    parser.addOption({"since", "Only since then: 30m, 2h, 1d, or a date and time like 2026-11-14T21:00.", "when"});
    parser.addOption({"grep", "Only lines that hold these words.", "words"});
    parser.addOption({"path", "Only say where the log file is."});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = "omabook logs";
    parser.process(rest);
    if (!parser.positionalArguments().isEmpty()) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    const bool json = parser.isSet("json");
    if (parser.isSet("path")) {
        if (json)
            printJson({{"path", Log::path()}});
        else
            printLine(stdout, Log::path());
        return 0;
    }
    if (const auto why = Numbers::problem(parser, {{.name = QStringLiteral("lines")}}))
        return failLine(*why);
    Log::Filter filter;
    filter.lines = parser.value("lines").toInt();
    filter.least = parser.value("level").toLower();
    filter.category = parser.value("category").toLower();
    filter.words = parser.value("grep");
    if (const QString since = parser.value("since"); !since.isEmpty()) {
        const auto when = sinceWhen(since);
        if (!when)
            return failLine(when.error());
        filter.since = *when;
    }
    static const QStringList levels{"debug", "info", "warning", "critical"};
    if (!levels.contains(filter.least))
        return failLine("--level is one of debug, info, warning, critical.");
    const QList<Log::Entry> entries = Log::read(filter);
    if (json) {
        QJsonArray array;
        for (const Log::Entry &entry : entries)
            array << entry.toJson();
        printJson({{"path", Log::path()}, {"entries", array}});
        return 0;
    }
    for (const Log::Entry &entry : entries)
        printLine(stdout, QString("%1 %2 %3 %4  %5")
                              .arg(entry.when.toString("yyyy-MM-dd HH:mm:ss"), entry.level.leftJustified(8), entry.program.leftJustified(6),
                                   entry.category.leftJustified(8), entry.message));
    if (entries.isEmpty())
        printLine(stderr, "Nothing in the log matches. It is kept at " + Log::path() + ".");
    return 0;
}
