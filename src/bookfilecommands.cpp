#include "bookfilecommands.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <expected>
#include <memory>
#include <print>
#include <tuple>

#include "bookadmission.h"
#include "formats.h"
#include "library.h"
#include "numberoptions.h"
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

// Reads the arguments after the command's name; false, with the help printed, for too few of them.
[[nodiscard]] bool parsed(QCommandLineParser &parser, const QStringList &arguments, const QString &name, qsizetype atLeast,
                          qsizetype atMost = -1) {
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook "_s + name;
    parser.process(rest);
    const qsizetype given = parser.positionalArguments().size();
    if (given < atLeast || (atMost >= 0 && given > atMost)) {
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

// The books asked for: those named, or with --all every one with an EPUB.
[[nodiscard]] QList<qint64> booksAsked(const Library &library, const QCommandLineParser &parser) {
    QList<qint64> ids;
    if (parser.isSet("all")) {
        for (const Book &book : library.books())
            if (book.formats.contains(u"epub"_s))
                ids << book.id;
        return ids;
    }
    for (const QString &given : parser.positionalArguments())
        ids << given.toLongLong();
    return ids;
}

// One book's file written or put back, as the command says it: {"id", and
// "written" with its "hash", "restored", or "error"}.
[[nodiscard]] QJsonObject embedOne(Library &library, qint64 id, bool restore) {
    QJsonObject said{{"id", id}};
    if (restore) {
        const auto restored = library.restoreFile(id);
        said.insert(restored ? u"restored"_s : u"error"_s, restored ? QJsonValue(true) : QJsonValue(restored.error().text()));
        return said;
    }
    const auto written = library.writeDetails(id);
    if (!written) {
        said.insert("error", written.error().text());
        return said;
    }
    said.insert("written", written->changed);
    said.insert("hash", written->hash);
    return said;
}

[[nodiscard]] QString embeddedLine(const QJsonObject &said) {
    const QString id = QString::number(said.value("id").toInteger());
    if (said.contains("error"))
        return id + u": "_s + said.value("error").toString();
    if (said.contains("restored"))
        return id + u": its file is back as it came in"_s;
    return id + (said.value("written").toBool() ? u": its details are in its EPUB now"_s : u": its EPUB said all of it already"_s);
}

} // namespace

int runAddFormats(Library &library, qint64 id, const QStringList &files, bool replace, bool json) {
    const auto same = replace ? Library::SameFormat::Trash : Library::SameFormat::Refuse;
    for (const QString &file : files) {
        const QString format = Formats::extension(file);
        const QString before = Library::hashFile(library.formatPath(id, format));
        const auto added = BookAdmission::addFormat(library, id, file, same);
        if (!added)
            return failLine(added.error().text());
        if (*added)
            printLine(stderr, (*added)->text());
        // Said beside the answer, which an agent is handed too.
        if (!before.isEmpty() && Library::hashFile(library.formatPath(id, format)) != before)
            printLine(stderr, u"Replaced the %1 file of book %2; the one it had is in the trash."_s.arg(Formats::name(format)).arg(id));
    }
    if (const auto book = library.book(id); book && json)
        printJson(book->toJson());
    return 0;
}

int runExportCli(const QStringList &arguments) {
    QCommandLineParser parser;
    common(parser, u"Copy a book's file out of the library into a folder."_s);
    parser.addPositionalArgument("id", "The book's number.");
    parser.addPositionalArgument("folder", "Where to put the copy.");
    parser.addOption({"format", "Which of the book's formats; the one the reader prefers when not given.", "format"});
    if (!parsed(parser, arguments, u"export"_s, 2, 2))
        return 1;
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const QStringList given = parser.positionalArguments();
    if (const auto why = Numbers::notNumbers(given.first(1)))
        return failLine(*why);
    const QString source = (*library)->formatPath(given.at(0).toLongLong(), parser.value("format"));
    if (source.isEmpty())
        return failLine(u"There is no such book or format in the library."_s);
    const QString target = QDir(given.at(1)).absoluteFilePath(QFileInfo(source).fileName());
    if (QFileInfo::exists(target))
        return failLine("There is already a file at " + target);
    if (!QDir().mkpath(given.at(1)) || !QFile::copy(source, target))
        return failLine("The copy can't be written to " + target);
    if (parser.isSet("json"))
        printJson(QJsonObject{{"file", target}});
    else
        printLine(stdout, target);
    return 0;
}

int runEmbedCli(const QStringList &arguments) {
    QCommandLineParser parser;
    common(parser, u"Write books' details (title, authors, series, identifiers, tags, description, cover...) into their EPUBs, "
                   "the library's own copies, so other apps and e-readers see them. The file you added is never touched, and "
                   "the library's copy as it came in is kept, to put back with --restore."_s);
    parser.addPositionalArgument("id", "The books' numbers.", "<id>...");
    parser.addOption({"all", "Every book of the library that has an EPUB."});
    parser.addOption({"restore", "Put each book's EPUB back as it came in, before details were written into it."});
    if (!parsed(parser, arguments, u"embed"_s, 0))
        return 1;
    if (!parser.isSet("all") && parser.positionalArguments().isEmpty())
        return failLine(u"Name the books by their numbers, or give --all."_s);
    if (const auto why = Numbers::notNumbers(parser.positionalArguments()))
        return failLine(*why);
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    QJsonArray books;
    bool failed = false;
    for (const qint64 id : booksAsked(**library, parser)) {
        const QJsonObject said = embedOne(**library, id, parser.isSet("restore"));
        failed = failed || said.contains("error");
        books << said;
        if (!parser.isSet("json"))
            printLine(said.contains("error") ? stderr : stdout, embeddedLine(said));
    }
    if (parser.isSet("json"))
        printJson(QJsonObject{{"books", books}});
    return failed ? 1 : 0;
}

void addDescribingOptions(QCommandLineParser &parser) {
    parser.addOption({"title", "The title of a --paper or --wish book.", "text"});
    parser.addOption({"authors", "Its authors; separate several with \" & \".", "text"});
    parser.addOption({"isbn", "Its ISBN.", "text"});
    QCommandLineOption globalId("global-id", "With --meta: the book's id on the computer it comes from.", "id");
    QCommandLineOption original("original", "With --meta: the hash its file came in with there.", "hash");
    for (QCommandLineOption *option : {&globalId, &original}) {
        option->setFlags(QCommandLineOption::HiddenFromHelp);
        parser.addOption(*option);
    }
}
