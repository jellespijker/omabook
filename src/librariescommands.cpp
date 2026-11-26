#include "librariescommands.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <print>
#include <tuple>

#include "libraries.h"
#include "library.h"
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

// Why the arguments can't be meant; empty when they can. An empty folder
// would be the current one (#234).
[[nodiscard]] QString refusal(const QCommandLineParser &parser) {
    if (!parser.positionalArguments().isEmpty())
        return parser.helpText().trimmed();
    for (const QString &option : {u"use"_s, u"forget"_s, u"trash"_s})
        if (parser.isSet(option) && parser.value(option).isEmpty())
            return u"--%1 needs a folder."_s.arg(option);
    return {};
}

} // namespace

int runLibrariesCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(u"List the libraries Omabook knows, or make another the one in use."_s);
    parser.addHelpOption();
    parser.addOption({u"json"_s, u"Print the result as JSON."_s});
    parser.addOption({u"use"_s, u"Make the library in this folder the one in use."_s, u"folder"_s});
    parser.addOption({u"create"_s, u"With --use: start a new library in a folder that isn't one yet."_s});
    parser.addOption({u"forget"_s, u"Take the library in this folder off the list; the folder stays."_s, u"folder"_s});
    parser.addOption({u"trash"_s, u"Take it off the list and move its folder, books and all, to the trash."_s, u"folder"_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook libraries"_s;
    parser.process(rest);
    if (const QString why = refusal(parser); !why.isEmpty())
        return failLine(why);
    Settings settings;
    QString problem;
    if (parser.isSet(u"forget"_s) && !Libraries::forget(settings, parser.value(u"forget"_s), &problem))
        return failLine(problem);
    if (parser.isSet(u"trash"_s) && !Libraries::trash(settings, parser.value(u"trash"_s), &problem))
        return failLine(problem);
    if (parser.isSet(u"use"_s)) {
        const QString folder = QDir(parser.value(u"use"_s)).absolutePath();
        if (!QFileInfo::exists(Library::databaseIn(folder)) && !parser.isSet(u"create"_s))
            return failLine(u"There is no library in %1. Add --create to start one there."_s.arg(folder));
        Library opened(folder);
        QString error;
        if (!opened.createIfMissing(parser.isSet(u"create"_s)).open(&error))
            return failLine(error);
        settings.setLibraryPath(folder);
    }
    const bool json = parser.isSet(u"json"_s);
    QJsonArray array;
    for (const QString &folder : settings.libraries()) {
        const bool current = folder == settings.libraryPath();
        const bool exists = QFileInfo::exists(Library::databaseIn(folder));
        array << QJsonObject{{"folder", folder}, {"current", current}, {"exists", exists}};
        if (!json)
            printLine(stdout, u"%1 %2%3"_s.arg(current ? u"*"_s : u" "_s, folder, exists ? QString() : u"  (not there)"_s));
    }
    if (json)
        printLine(stdout,
                  QString::fromUtf8(
                      QJsonDocument(QJsonObject{{"current", settings.libraryPath()}, {"libraries", array}}).toJson(QJsonDocument::Indented))
                      .trimmed());
    return 0;
}
