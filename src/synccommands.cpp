#include "synccommands.h"

#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <cstdio>
#include <expected>
#include <memory>
#include <print>

#include "library.h"
#include "librarymigrations.h"
#include "pluginfiles.h"
#include "pluginfilesync.h"
#include "remote.h"
#include "settings.h"
#include "sync.h"
#include "synclog.h"

namespace {

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    fflush(stream);
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

void printJsonObject(const QJsonObject &object) {
    printLine(stdout, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
}

void addCommonOptions(QCommandLineParser &parser, const QString &description) {
    parser.setApplicationDescription(description);
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library",
                      "Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use).",
                      "folder"});
}

bool parseArguments(QCommandLineParser &parser, const QString &name, const QStringList &arguments, int atMost) {
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = "omabook " + name;
    parser.process(rest);
    if (parser.positionalArguments().size() <= atMost)
        return true;
    printLine(stderr, parser.helpText().trimmed());
    return false;
}

[[nodiscard]] std::expected<std::unique_ptr<Library>, QString> openCliLibrary(const QCommandLineParser &parser) {
    auto library = std::make_unique<Library>(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    library->onBringingUpToDate(LibraryMigrations::sayOnStandardError);
    QString error;
    if (!library->open(&error))
        return std::unexpected(error);
    return library;
}

[[nodiscard]] std::expected<SyncOptions, QString> parseSyncOptions(const QCommandLineParser &parser) {
    SyncOptions options;
    options.dryRun = parser.isSet("dry-run");
    const QString details = parser.value("details").toLower();
    if (details == "mine")
        options.details = SyncOptions::Details::Mine;
    else if (details == "theirs")
        options.details = SyncOptions::Details::Theirs;
    else if (parser.isSet("details"))
        return std::unexpected(QStringLiteral("--details is mine or theirs."));
    return options;
}

void printSection(const QString &what, const QStringList &titles) {
    if (titles.isEmpty())
        return;
    printLine(stdout, QString("%1 (%2):").arg(what).arg(titles.size()));
    for (const QString &title : titles)
        printLine(stdout, "  " + title);
}

void printDryRun(const SyncSummary &summary, const QString &host) {
    printLine(stdout, QString("A dry run: nothing was changed, here or on %1. A sync would do this:").arg(host));
    const SyncSummary::Titles &titles = summary.titles;
    printSection("Received here", titles.received);
    printSection("Sent to " + host, titles.sent);
    printSection("Details taken here", titles.detailsTaken);
    printSection("Details given to " + host, titles.detailsSent);
    printSection("Reading places taken here", titles.placesTaken);
    printSection("Reading places given to " + host, titles.placesSent);
    printSection("Shelf, stars or finished day taken here", titles.readingTaken);
    printSection("Shelf, stars or finished day given to " + host, titles.readingSent);
    printSection("Removed here, to the trash", titles.removedHere);
    printSection("Removed on " + host + ", to the trash", titles.removedThere);
    printSection("Details that differ, with no time to say which are newer", titles.detailsUndecided);
    const int total = summary.received + summary.sent + summary.detailsTaken + summary.detailsSent + summary.placesTaken
                      + summary.placesSent + summary.readingTaken + summary.readingSent + summary.removedHere + summary.removedThere
                      + summary.detailsUndecided;
    if (total == 0)
        printLine(stdout, "Nothing: both libraries are in step.");
}

void printPlainSummary(const SyncSummary &summary, const QString &host) {
    printLine(stdout, QString("%1 received, %2 sent, %3 reading places updated, %4 details updated, %5 removed here, %6 removed on %7, "
                              "%8 graph changes taken, %9 graph changes sent, %10 bookmarks and highlights updated")
                          .arg(summary.received)
                          .arg(summary.sent)
                          .arg(summary.placesTaken + summary.placesSent)
                          .arg(summary.detailsTaken + summary.detailsSent)
                          .arg(summary.removedHere)
                          .arg(summary.removedThere)
                          .arg(host)
                          .arg(summary.graphTaken)
                          .arg(summary.graphSent)
                          .arg(summary.annotationsTaken + summary.annotationsSent));
    if (summary.pluginFilesTaken + summary.pluginFilesSent > 0)
        printLine(stdout, QString("%1 files of plugins received, %2 sent").arg(summary.pluginFilesTaken).arg(summary.pluginFilesSent));
    if (summary.pluginFilesHeld > 0)
        printLine(stdout, QString("%1 files of plugins stay where they are: over the %2 MB a file, or the %3 MB a plugin, that travel")
                              .arg(summary.pluginFilesHeld)
                              .arg(PluginFiles::maxFileBytes / (qint64{1024} * 1024))
                              .arg(PluginFiles::maxPluginBytes / (qint64{1024} * 1024)));
}

int printSyncStatus(const QCommandLineParser &parser, const Library &library, const QString &computer) {
    const bool ssh = !QStandardPaths::findExecutable("ssh").isEmpty();
    if (parser.isSet("json"))
        printJsonObject(QJsonObject{{"computer", computer},
                                    {"configured", !computer.isEmpty()},
                                    {"ssh_installed", ssh},
                                    {"library", library.root()},
                                    {"books", library.books().size()}});
    else
        printLine(stdout, computer.isEmpty() ? QString("No computer to sync with is named in the settings.")
                                             : QString("Syncs with %1%2").arg(computer, ssh ? "" : " (ssh is not installed)"));
    return 0;
}

int printSyncResult(const SyncSummary &summary, const QString &computer, bool json) {
    const QString host = Remote(computer).host();
    if (json)
        printJsonObject(summary.toJson());
    else if (summary.dryRun)
        printDryRun(summary, host);
    else
        printPlainSummary(summary, host);
    if (summary.detailsUndecided > 0 && !json)
        printLine(stdout,
                  QString("%1 other details on %2, and neither side knows which are newer, so they were left "
                          "as they are. To settle them: omabook sync %3 --details mine (keep this computer's) or "
                          "--details theirs (take %2's); add --dry-run to see first.")
                      .arg(summary.detailsUndecided == 1 ? QString("1 book has") : QString("%1 books have").arg(summary.detailsUndecided))
                      .arg(host, computer));
    for (const QString &problem : summary.problems)
        printLine(stderr, problem);
    return summary.problems.isEmpty() ? 0 : 1;
}

int answerReplication(const QCommandLineParser &parser) {
    QFile file(parser.value("replicate"));
    if (!file.open(QIODevice::ReadOnly))
        return failLine(parser.value("replicate") + " can't be read.");
    const QJsonObject request = QJsonDocument::fromJson(file.readAll()).object();
    const auto library = openCliLibrary(parser);
    if (!library)
        return failLine(library.error());
    printJsonObject(SyncLog::answer(**library, request));
    return 0;
}

// What omabook sync asks of this computer for the files of plugins' data/
// folders (PluginFileSync): what they hold, or to take those in a request.
int answerPluginFiles(const QCommandLineParser &parser) {
    const auto library = openCliLibrary(parser);
    if (!library)
        return failLine(library.error());
    if (!parser.isSet("take-plugin-files")) {
        printJsonObject(PluginFileSync::survey(**library));
        return 0;
    }
    QFile file(parser.value("take-plugin-files"));
    if (!file.open(QIODevice::ReadOnly))
        return failLine(parser.value("take-plugin-files") + " can't be read.");
    printJsonObject(PluginFileSync::take(**library, QJsonDocument::fromJson(file.readAll()).object()));
    return 0;
}

} // namespace

int runManifestCli(const QStringList &arguments) {
    QCommandLineParser parser;
    addCommonOptions(parser, "Print everything the library holds as JSON: each book's details, files with their "
                             "hashes, cover and reading place. This is what omabook sync reads from the other computer.");
    parser.addOption({"log", "For an Omabook that speaks the change log: say so, and leave out what the log carries "
                             "(all but what names each book, and its files and cover)."});
    if (!parseArguments(parser, "manifest", arguments, 0))
        return 1;
    const auto library = openCliLibrary(parser);
    if (!library)
        return failLine(library.error());
    printJsonObject(libraryManifest(**library, parser.isSet("log") ? ManifestFor::LogPeer : ManifestFor::OlderPeer));
    return 0;
}

int runSyncCli(const QStringList &arguments) {
    QCommandLineParser parser;
    addCommonOptions(parser, "Bring this library and another computer's in step over ssh: each gets the books it lacks, "
                             "the newer details and the newer reading place win, and a book removed on one side is removed "
                             "on the other too (to the trash), unless it changed there since. Omabook must be installed on both.");
    parser.addPositionalArgument("computer",
                                 "The other computer as ssh knows it (a Tailscale name works), optionally "
                                 "with its library folder: laptop or laptop:Books.",
                                 "<computer[:folder]>");
    parser.addOption({"status", "Only say whether syncing is set up, and with which computer."});
    parser.addOption({"details",
                      "For books whose details differ and no time says which are newer (books from before "
                      "Omabook kept that time): take this computer's (mine) or the other's (theirs).",
                      "mine|theirs"});
    parser.addOption({"dry-run", "Change nothing on either side; list what would be received, sent, updated and removed on each."});
    parser.addOption({"replicate",
                      "What omabook sync asks of this computer from the other one: take in the change log's operations in "
                      "this file, and print the ones the other computer lacks.",
                      "file"});
    parser.addOption({"plugin-files", "What omabook sync asks of this computer: record what plugins' data/ folders hold, and print it."});
    parser.addOption({"take-plugin-files",
                      "What omabook sync asks of this computer: take in the files of plugins' data/ folders this file names.", "file"});
    if (!parseArguments(parser, "sync", arguments, 1))
        return 1;
    if (parser.isSet("replicate"))
        return answerReplication(parser);
    if (parser.isSet("plugin-files") || parser.isSet("take-plugin-files"))
        return answerPluginFiles(parser);
    const auto options = parseSyncOptions(parser);
    if (!options)
        return failLine(options.error());
    const auto library = openCliLibrary(parser);
    if (!library)
        return failLine(library.error());
    const QString argComputer = parser.positionalArguments().value(0);
    const QString computer = argComputer.isEmpty() ? Settings().syncTarget() : argComputer;
    if (parser.isSet("status"))
        return printSyncStatus(parser, **library, computer);
    if (computer.isEmpty())
        return failLine("Name the computer to sync with, or set one in the settings.");
    const bool json = parser.isSet("json");
    const SyncSummary summary = syncLibrary(
        **library, Remote(computer),
        [json](const QString &title) {
            if (!json)
                printLine(stderr, title);
        },
        *options);
    if (!summary.error.isEmpty())
        return failLine(summary.error);
    return printSyncResult(summary, computer, json);
}
