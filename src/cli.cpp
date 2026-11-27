#include "cli.h"

#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSaveFile>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <memory>
#include <termios.h>
#include <unistd.h>

#include "aicommands.h"
#include "airegistry.h"
#include "aisearch.h"
#include "annotationcommands.h"
#include "bookfilecommands.h"
#include "bookmarkcommands.h"
#include "bookops.h"
#include "booksetting.h"
#include "calibreplugin.h"
#include "catalog.h"
#include "chronologycommands.h"
#include "clioutput.h"
#include "collections.h"
#include "curation.h"
#include "customization.h"
#include "devicecommands.h"
#include "findcommands.h"
#include "folio.h"
#include "formats.h"
#include "goodreads.h"
#include "graph.h"
#include "graphcontext.h"
#include "graphtools.h"
#include "hubcommands.h"
#include "identitycommands.h"
#include "importer.h"
#include "interfacechange.h"
#include "knowledgesearch.h"
#include "librariescommands.h"
#include "library.h"
#include "logcommands.h"
#include "manual.h"
#include "mcp.h"
#include "metadatasource.h"
#include "numberoptions.h"
#include "plugins.h"
#include "prompts.h"
#include "readingcommands.h"
#include "readingformat.h"
#include "readingsite.h"
#include "scriptapi.h"
#include "scriptplugins.h"
#include "scripttools.h"
#include "secretstore.h"
#include "settings.h"
#include "sitedesk.h"
#include "snapshotcommands.h"
#include "spacecommands.h"
#include "synccommands.h"
#include "theme.h"
#include "themechoice.h"

namespace {

using namespace Qt::StringLiterals;
using CliOutput::fail;
using CliOutput::print;
using CliOutput::printJson;

// One command's options, with the two every command takes.
struct Command {
    Command(const QString &name, const QString &description) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addOption({"json", "Print the result as JSON."});
        parser.addOption({"library", "Use the library in this folder instead of the current one, for this run only "
                                     "(omabook libraries --use changes the one in use).", "folder"});
        this->name = name;
    }

    // False when the arguments were wrong, with the reason already printed; so
    // is an option among `numbers` that isn't the number it takes (#230).
    bool parse(const QStringList &arguments, int atLeast, int atMost = -1, std::initializer_list<Numbers::Option> numbers = {}) {
        QStringList rest = arguments;
        rest.removeAt(1);
        rest[0] = "omabook " + name;
        parser.process(rest);
        const int given = parser.positionalArguments().size();
        if (given < atLeast || (atMost >= 0 && given > atMost)) {
            print(stderr, parser.helpText().trimmed());
            return false;
        }
        if (const auto why = Numbers::problem(parser, numbers)) {
            print(stderr, *why);
            return false;
        }
        return true;
    }

    bool json() const { return parser.isSet("json"); }
    QString argument(int index) const { return parser.positionalArguments().value(index); }

    std::unique_ptr<Library> library(QString *error) const {
        auto library = std::make_unique<Library>(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
        library->onBringingUpToDate([](const QString &said) { print(stderr, said); });
        return library->open(error) ? std::move(library) : nullptr;
    }

    // Whether the library worked on is the one in use. What is remembered
    // in the settings about a library's books (which Calibre libraries were
    // imported into it) is remembered only then: --library is for one run,
    // and leaves the settings as they were.
    bool libraryInUse(const Settings &settings) const {
        return !parser.isSet("library")
               || QDir::cleanPath(QDir(parser.value("library")).absolutePath()) == QDir::cleanPath(settings.libraryPath());
    }

    QString name;
    QCommandLineParser parser;
};

// A book named on the command line: its number in the library, or any file.
struct Target {
    qint64 id = 0;
    QString file;
    QString error;
};

Target resolve(Library *library, const QString &word) {
    Target target;
    bool number = false;
    target.id = word.toLongLong(&number);
    if (!number) {
        target.id = 0;
        target.file = QFileInfo(word).absoluteFilePath();
        if (!QFileInfo::exists(target.file)) // a file, or a folder that is one book
            target.error = "There is no book or file called " + word + ".";
        return target;
    }
    if (!library || !library->book(target.id)) {
        target.error = QString("There is no book %1 in the library.").arg(target.id);
        return target;
    }
    const QStringList formats = library->book(target.id)->formats;
    for (const QString &format : formats)
        if (target.file.isEmpty() && Formats::readable(format))
            target.file = library->formatPath(target.id, format);
    if (target.file.isEmpty())
        target.error = QString("Book %1 has no format Omabook reads itself; run: omabook convert %1").arg(target.id);
    return target;
}

std::unique_ptr<Document> openTarget(const Command &command, const QString &word, QString *error) {
    QString libraryError;
    const auto library = command.library(&libraryError);
    const Target target = resolve(library.get(), word);
    if (!target.error.isEmpty()) {
        *error = library || !target.file.isEmpty() ? target.error : libraryError; // why it can't be opened (#227)
        return nullptr;
    }
    return Formats::open(target.file, error);
}

int list(const QStringList &arguments) {
    Command command("list", "List the library's books, or those matching a search.");
    command.parser.addPositionalArgument("search", "Words to look for; author:, title:, series:, tag:, format: and lang: look in one field.", "[search...]");
    command.parser.addOption({"collection", "Only the books of this collection (omabook collections lists them).", "name"});
    if (!command.parse(arguments, 0))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    QList<Book> books = library->books(command.parser.positionalArguments().join(' '));
    if (command.parser.isSet("collection")) {
        const auto collection = library->collectionNamed(command.parser.value("collection"));
        if (!collection)
            return fail("There is no collection called " + command.parser.value("collection") + ". omabook collections lists them.");
        books = collection->of(books);
    }
    if (command.json()) {
        QJsonArray array;
        for (const Book &book : books)
            array << book.toJson();
        printJson(array);
    } else {
        for (const Book &book : books)
            print(stdout, QString("%1\t%2\t%3\t%4").arg(book.id).arg(book.meta.title, book.meta.authorText(), book.formats.join(',')));
    }
    return 0;
}

int search(const QStringList &arguments) {
    Command command("search", "Find books as the window's search does: by their details and by what the knowledge graph "
                              "knows of them, as far as you have read. With --ai, the AI looks through the library for a "
                              "question no search can put, and answers with the books and a reason each.");
    command.parser.addPositionalArgument("words", "A search: bare words also find books by the characters, places, ideas and "
                                                  "questions met in them; knows: looks only there. With --ai, a question "
                                                  "in your own words.",
                                         "<words...>");
    command.parser.addOption({"ai", "Ask the AI, a batch of books at a time; an answer is kept, so asking again doesn't ask again."});
    command.parser.addOption({"collection", "Only the books of this collection (omabook collections lists them).", "name"});
    if (!command.parse(arguments, 1))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    QList<Book> books = library->books();
    qint64 collectionId = 0;
    if (command.parser.isSet("collection")) {
        const auto collection = library->collectionNamed(command.parser.value("collection"));
        if (!collection)
            return fail("There is no collection called " + command.parser.value("collection") + ". omabook collections lists them.");
        books = collection->of(books);
        collectionId = collection->id;
    }
    const QString words = command.parser.positionalArguments().join(' ');
    // What is known of the books, unless the graph is switched off.
    const auto known = std::make_shared<const KnowledgeIndex>(
        Settings().graph() == "off" ? KnowledgeIndex() : KnowledgeIndex::forSearch(*library, words, command.parser.isSet("ai")));
    if (!command.parser.isSet("ai")) {
        const QList<KnowledgeSearch::Found> found = KnowledgeSearch::search(*library, books, words, known);
        if (command.json()) {
            QJsonArray array;
            for (const KnowledgeSearch::Found &one : found) {
                QJsonObject object = one.book.toJson();
                if (one.why)
                    object.insert("known", one.why->toJson());
                array << object;
            }
            printJson(array);
        } else {
            for (const KnowledgeSearch::Found &one : found)
                print(stdout, QString("%1\t%2\t%3\t%4")
                                  .arg(one.book.id)
                                  .arg(one.book.meta.title, one.book.meta.authorText(),
                                       one.why ? one.why->name + " (" + one.why->kind + ")" : QString()));
        }
        return 0;
    }

    const QString key = AiSearch::cacheKey(*library, words, collectionId);
    std::optional<QList<AiSearch::Pick>> picks = AiSearch::cached(key);
    const bool cached = picks.has_value();
    int requests = 0;
    if (!picks) {
        const Settings settings;
        QString why;
        if (!AiRegistry::answerer(settings)(&why))
            return fail(why);
        AiSearch::Run run(AiRegistry::answerer(settings), words, books, known);
        requests = run.total();
        if (!command.json() && requests > 1)
            print(stderr, QString("Looking through %1 books, %2 requests…").arg(run.books()).arg(requests));
        QEventLoop loop;
        QString failure;
        QObject::connect(&run, &AiSearch::Run::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
        QObject::connect(&run, &AiSearch::Run::failed, &loop, [&failure, &loop](const QString &problem) {
            failure = problem;
            loop.quit();
        }, Qt::QueuedConnection);
        run.start();
        loop.exec();
        if (!failure.isEmpty())
            return fail(failure);
        picks = run.picks();
        AiSearch::remember(key, *picks);
    }
    if (command.json()) {
        QJsonArray array;
        for (const AiSearch::Pick &pick : *picks)
            array << AiSearch::toJson(pick, library->book(pick.bookId));
        printJson(QJsonObject{{"question", words}, {"books", array}, {"cached", cached}, {"requests", requests}});
    } else {
        if (picks->isEmpty())
            print(stdout, "No book answers that.");
        for (const AiSearch::Pick &pick : *picks)
            if (const auto book = library->book(pick.bookId))
                print(stdout, QString("%1\t%2\t%3\t%4").arg(pick.bookId).arg(book->meta.title, book->meta.authorText(), pick.why));
    }
    return 0;
}

int show(const QStringList &arguments) {
    Command command("show", "Show everything the library knows about one book.");
    command.parser.addPositionalArgument("id", "The book's number, from omabook list.");
    if (!command.parse(arguments, 1, 1))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const auto book = library->book(command.argument(0).toLongLong());
    if (!book)
        return fail("There is no book " + command.argument(0) + " in the library.");
    QJsonObject object = book->toJson();
    QJsonObject files;
    for (const QString &format : book->formats)
        files.insert(format, library->formatPath(book->id, format));
    object.insert("files", files);
    object.insert("cover", library->coverPath(book->id));
    object.insert("identifiers", book->identifiers);
    if (command.json()) {
        printJson(object);
    } else {
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QJsonValue value = it.value();
            QString text = value.isArray() ? QString() : value.toVariant().toString();
            for (const QJsonValue &item : value.toArray())
                text += (text.isEmpty() ? "" : ", ") + item.toString();
            // Files by their format alone; identifiers as scheme=value.
            const QJsonObject inside = value.toObject();
            for (auto item = inside.begin(); item != inside.end(); ++item)
                text += (text.isEmpty() ? "" : "\n\t") + (it.key() == "identifiers" ? item.key() + "=" : QString())
                        + item.value().toVariant().toString();
            if (!text.isEmpty())
                print(stdout, it.key() + ":\t" + text);
        }
    }
    return 0;
}

int add(const QStringList &arguments) {
    Command command("add", "Copy books into the library. The files you name are left as they are. A folder is searched for "
                           "books; a Calibre library is taken with Calibre's own details, covers and every format.");
    command.parser.addPositionalArgument("files", "Book files, folders, or a Calibre library.", "<file|folder>...");
    command.parser.addOption({"to", "Add the file to this book as another format, instead of as a new book.", "id"});
    command.parser.addOption({"replace", "With --to: put the file in place of the book's file of that format, which goes to the trash."});
    command.parser.addOption({"meta", "Take the book's details from this JSON file (as omabook show --json prints them).", "file"});
    command.parser.addOption({"cover", "Use this picture as the cover.", "file"});
    command.parser.addOption({"paper", "Add a book that is on a real shelf and has no file. Describe it with --title and "
                                       "--authors, or with --meta."});
    command.parser.addOption({"wish", "Add a book you don't have, to read some day. Described like --paper."});
    addDescribingOptions(command.parser);
    const bool fileless = arguments.contains("--paper") || arguments.contains("--wish");
    if (!command.parse(arguments, fileless ? 0 : 1))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const QStringList paths = command.parser.positionalArguments();
    if (command.parser.isSet("replace") && !command.parser.isSet("to"))
        return fail("--replace goes with --to.");

    if (fileless) {
        if (!paths.isEmpty())
            return fail("--paper and --wish add a book with no file: name none.");
        BookMeta meta;
        if (command.parser.isSet("meta")) {
            QFile details(command.parser.value("meta"));
            if (!details.open(QIODevice::ReadOnly))
                return fail("The details can't be read: " + command.parser.value("meta"));
            meta = BookMeta::fromJson(QJsonDocument::fromJson(details.readAll()).object());
        }
        if (command.parser.isSet("title")) meta.title = command.parser.value("title").simplified();
        if (command.parser.isSet("authors")) meta.authors = BookMeta::splitAuthors(command.parser.value("authors"));
        if (command.parser.isSet("isbn")) meta.isbn = command.parser.value("isbn").simplified();
        if (command.parser.isSet("cover")) {
            QImageReader cover(command.parser.value("cover"));
            cover.setAutoTransform(true);
            meta.cover = cover.read();
        }
        const Library::Added added = library->addEntry(meta, command.parser.isSet("paper"));
        if (!added.error.isEmpty())
            return fail(added.error);
        if (command.parser.isSet("wish"))
            library->setReading(added.id, "to-read", 0, {});
        if (command.json())
            printJson(library->book(added.id)->toJson());
        else
            print(stdout, QString("Added book %1: %2").arg(added.id).arg(meta.title));
        return 0;
    }

    if (command.parser.isSet("to"))
        return runAddFormats(*library, command.parser.value("to").toLongLong(), paths, command.parser.isSet("replace"), command.json());

    ImportSummary summary;
    if (command.parser.isSet("meta") || command.parser.isSet("cover")) {
        // One book, described by whoever is adding it.
        if (paths.size() != 1)
            return fail("--meta and --cover describe one book: name one file.");
        std::optional<BookMeta> meta;
        if (command.parser.isSet("meta")) {
            QFile details(command.parser.value("meta"));
            if (!details.open(QIODevice::ReadOnly))
                return fail("The details can't be read: " + command.parser.value("meta"));
            meta = BookMeta::fromJson(QJsonDocument::fromJson(details.readAll()).object());
        } else {
            meta = Formats::readMeta(paths.first());
        }
        if (command.parser.isSet("cover")) {
            QImageReader cover(command.parser.value("cover"));
            cover.setAutoTransform(true);
            meta->cover = cover.read();
        }
        summary = importAsThere(*library, paths.first(), *meta, command.parser.value("global-id"), command.parser.value("original"));
    } else {
        // Whether a Calibre library is read as one is the Calibre plugin's to say.
        Settings settings;
        const CalibrePlugin calibre(&settings);
        summary = importBooks(*library, paths, {}, calibre.importOptions());
        for (const QString &path : paths)
            if (calibre.switchedOff() && QFileInfo(path + "/metadata.db").isFile())
                print(stderr, "The Calibre plugin is switched off, so " + path + " was added as a plain folder of books. "
                              "Switch it on with: omabook plugins enable calibre");
        if (command.libraryInUse(settings))
            for (const QString &folder : std::as_const(summary.calibreLibraries))
                settings.setCalibreLibrary(QDir(folder).absolutePath(), "imported");
    }

    for (const ImportSummary::Item &item : std::as_const(summary.items)) {
        if (!command.json() && item.error.isEmpty())
            print(stdout, QString("%1\t%2%3").arg(item.id).arg(library->book(item.id)->meta.title,
                                                               item.duplicate ? "\t(already in the library)"
                                                               : item.joined ? "\t(joined the book already listed)" : ""));
    }
    if (command.json())
        printJson(summary.itemsJson());
    for (const QString &reason : summary.reasons())
        print(stderr, reason);
    if (summary.foundNothing())
        return fail("No books were found there.");
    return summary.failed() ? 1 : 0;
}

// How books are set on the page: the defaults every book follows, or one
// book's own over them (readingformat.h).
int format(const QStringList &arguments) {
    Command command("format", "Show or change how books are set on the page: the type, the longest line (the measure, in "
                              "characters), the pages side by side, the spacing, the indent, the margins, the alignment, and "
                              "whether a book is read as pages or as one scrolling column. "
                              "Without a book, the defaults every book follows; with one, that book's own, which lie over the "
                              "defaults. Without options, what is in force and where each comes from. Pages that are pictures "
                              "(PDF, comics) are not set and ignore it, but for the layout and the first page.");
    command.parser.addPositionalArgument("book", "A book's number from omabook list, for its own; the defaults when not given.", "[book]");
    struct Flag {
        const char *name, *key, *what, *value;
    };
    static constexpr Flag flags[] = {
        {"font-size", "font_size", "The type size, in pixels", "N"},
        {"font", "font_family", "The font, by name; \"\" for the desktop's.", "font"},
        {"measure", "measure", "The longest line, in characters of the font (66 reads best; the type's size and font "
                               "decide its width in pixels)", "N"},
        {"line-height", "line_height", "Line spacing, in percent", "N"},
        {"paragraph-spacing", "paragraph_spacing", "The space after a paragraph, in pixels", "N"},
        {"indent", "indent", "The indent of a paragraph's first line, in pixels", "N"},
        {"margins", "margins", "The room around the text, in pixels", "N"},
        {"align", "align", "start, justify (filled out to both sides), center or end. Start and end turn round in a "
                           "right-to-left language.", "how"},
        {"layout", "layout", "How the book is read through: pages (cut into pages that turn, a chapter starting on a page "
                             "of its own; a PDF's page shown whole), scroll (one column that runs on into the next "
                             "chapter) or auto (pages where two or more fit side by side, else a column that scrolls). "
                             "auto when not set.", "how"},
        {"first-page", "first_page", "Pages that are pictures, two side by side: alone (the first page on its own, then "
                                     "2|3, as a printed book opens), paired (1|2, 3|4) or auto (alone when the first page "
                                     "is taller than wide, paired for slides). auto when not set.", "how"},
    };
    for (const Flag &flag : flags) {
        QString what = flag.what;
        if (const ReadingFormat::Range *range = ReadingFormat::range(QLatin1StringView(flag.key)))
            what += QString(": %1 to %2, %3 when not set.").arg(range->low).arg(range->high).arg(range->fallback);
        command.parser.addOption({flag.name, what, flag.value});
    }
    command.parser.addOption({"pages", "How many pages stand side by side in the reader, for every book: auto (as many as the "
                                       "window has room for, each with lines of at least 45 characters, up to three), 1, 2 or "
                                       "3 (shown where each page keeps 45 characters; the most that do otherwise).", "auto|1|2|3"});
    command.parser.addOption({"reset", "With a book: it follows the defaults again. Without: the defaults go back to Omabook's own."});
    if (!command.parse(arguments, 0, 1))
        return 1;

    Settings settings;
    std::unique_ptr<Library> library;
    qint64 id = 0;
    if (!command.argument(0).isEmpty()) {
        bool number = false;
        id = command.argument(0).toLongLong(&number);
        if (!number)
            return fail("<book> is a book's number, as omabook list gives it.");
        QString error;
        library = command.library(&error);
        if (!library)
            return fail(error);
        if (!library->book(id))
            return fail(QString("There is no book %1 in the library.").arg(id));
    }

    // Everything asked is checked before anything changes.
    QJsonObject changes;
    for (const Flag &flag : flags) {
        if (!command.parser.isSet(flag.name))
            continue;
        const QString key = flag.key, given = command.parser.value(flag.name);
        QJsonValue value = given;
        if (const ReadingFormat::Range *range = ReadingFormat::range(key)) {
            bool whole = false;
            const int number = given.toInt(&whole);
            if (!whole)
                return fail(QString("--%1 takes a whole number from %2 to %3.").arg(flag.name).arg(range->low).arg(range->high));
            value = number;
        }
        if (const QString why = ReadingFormat::problem(key, value); !why.isEmpty())
            return fail("--" + QString(flag.name) + why.mid(key.size()));
        changes.insert(key, value);
    }
    const bool reset = command.parser.isSet("reset");
    const bool pages = command.parser.isSet("pages");
    if (reset && (!changes.isEmpty() || pages))
        return fail("--reset goes alone: it puts everything back.");
    const QString pageCount = command.parser.value("pages");
    if (pages && pageCount != "auto" && pageCount != "1" && pageCount != "2" && pageCount != "3")
        return fail(QString("--pages is auto, 1, 2 or 3; %1 is not.").arg(pageCount));
    if (pages && id > 0)
        return fail("--pages is for every book; give it without a book.");

    if (id > 0) {
        QJsonObject own = reset ? QJsonObject() : library->bookFormat(id);
        for (auto it = changes.begin(); it != changes.end(); ++it)
            own.insert(it.key(), it.value());
        if ((reset || !changes.isEmpty()) && !library->setBookFormat(id, own))
            return fail("The library can't be written.");
    } else if (reset) {
        for (const ReadingFormat::Range &range : ReadingFormat::ranges)
            settings.setOption(range.key, range.fallback);
        settings.setOption("font_family", "");
        for (const ReadingFormat::Choice &choice : ReadingFormat::choices)
            settings.setOption(choice.key, QString(choice.fallback));
        settings.setPages(0);
    } else {
        for (auto it = changes.begin(); it != changes.end(); ++it)
            settings.setOption(it.key(), it.value());
        if (pages)
            settings.setOption("pages", pageCount);
    }

    const QJsonObject defaults = settings.format();
    const QJsonObject own = id > 0 ? library->bookFormat(id) : QJsonObject();
    const QJsonObject effective = ReadingFormat::over(defaults, own);
    if (command.json()) {
        QJsonObject result{{"effective", effective}, {"defaults", defaults}, {"bounds", ReadingFormat::bounds()},
                           {"pages", settings.pages() == 0 ? QJsonValue("auto") : QJsonValue(settings.pages())},
                           {"changed", reset || pages || !changes.isEmpty()}};
        if (id > 0) {
            result.insert("book", id);
            result.insert("title", library->book(id)->meta.title);
            result.insert("own", own);
        }
        printJson(result);
        return 0;
    }
    print(stdout, id > 0 ? QString("%1 is set like this; \"this book\" marks its own, the rest are the defaults:")
                               .arg(library->book(id)->meta.title)
                         : QString("Every book is set like this, unless it has a way of its own:"));
    for (const QString &key : ReadingFormat::keys()) {
        const QJsonValue value = effective.value(key);
        const ReadingFormat::Range *range = ReadingFormat::range(key);
        const QString shown = range ? QString::number(value.toInt()) + (range->unit == "%"_L1 ? QString("%") : " " + QString(range->unit))
                              : key == "font_family" && value.toString().isEmpty() ? QString("the desktop's")
                                                                                   : value.toString();
        print(stdout, QString("  %1 %2%3").arg(key.leftJustified(18), shown, own.contains(key) ? "   (this book)" : ""));
    }
    print(stdout, QString("  %1 %2").arg(QString("pages").leftJustified(18),
                                         settings.pages() == 0 ? QString("auto (as many as fit)") : QString::number(settings.pages())));
    return 0;
}

int remove(const QStringList &arguments) {
    Command command("remove", "Move a book's folder to the trash and take it out of the library.");
    command.parser.addPositionalArgument("id", "The book's number.");
    if (!command.parse(arguments, 1, 1))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (const auto removed = library ? library->remove(command.argument(0).toLongLong()) : std::unexpected(error); !removed)
        return fail(removed.error());
    if (command.json())
        printJson(QJsonObject{{"removed", command.argument(0).toLongLong()}});
    return 0;
}

int set(const QStringList &arguments) {
    Command command("set", "Change a book's details. Only the options given are changed.");
    command.parser.addPositionalArgument("id", "The book's number.");
    BookSetting::addOptions(command.parser);
    if (!command.parse(arguments, 1, 1))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const auto set = BookSetting::apply(*library, command.argument(0).toLongLong(), command.parser,
                                        [](const QString &said) { print(stderr, said); });
    if (!set)
        return fail(set.error().text());
    if (command.json())
        printJson(set->toJson());
    return 0;
}

int info(const QStringList &arguments) {
    Command command("info", "Show what a book file says about itself, without adding it to the library. Without a "
                            "file: Omabook's version, the library in use, and which AI answers and why.");
    command.parser.addPositionalArgument("file", "A book file.", "[file]");
    if (!command.parse(arguments, 0, 1))
        return 1;
    if (command.argument(0).isEmpty()) {
        const Settings settings;
        const AiChoice ai = Ai::choose(settings);
        const QJsonObject described = Ai::describe(settings);
        const QString library = command.parser.isSet("library") ? QDir(command.parser.value("library")).absolutePath()
                                                                : settings.libraryPath();
        const Customization custom;
        const ThemeSource theme = resolveTheme(custom.style().value("theme").toString(), ThemePlaces::here(custom.folder()));
        if (command.json()) {
            printJson(QJsonObject{{"version", OMABOOK_VERSION}, {"library", library},
                                  {"config_folder", custom.folder()}, {"ai", described}, {"theme", theme.toJson()}});
        } else {
            print(stdout, "version:\t" OMABOOK_VERSION);
            print(stdout, "library:\t" + library);
            print(stdout, "ai:\t" + (ai.available() ? ai.name : QString("none")));
            // An empty name is the provider's own default.
            auto named = [](const QString &model) { return model.isEmpty() ? QString("the default") : model; };
            if (ai.available()) {
                print(stdout, "\tfor answers and writing: " + named(described.value("main_model").toString()));
                print(stdout, "\tfor reading and parsing: " + named(described.value("quick_model").toString()));
            }
            if (ai.source == AiChoice::OmarchyDefault) print(stdout, "\tOmarchy's default agent");
            if (!ai.problem.isEmpty()) print(stdout, "\t" + ai.problem);
            print(stdout, "theme:\t" + theme.name + "\n\t" + theme.why);
        }
        return 0;
    }
    const QString file = command.argument(0);
    if (!QFileInfo(file).isFile())
        return fail("There is no such file: " + file);
    if (!Formats::isBook(file))
        return fail(QFileInfo(file).fileName() + " is not a book format Omabook knows.");
    const BookMeta meta = Formats::readMeta(file);
    QJsonObject object = meta.toJson();
    object.insert("format", Formats::name(Formats::extension(file)));
    object.insert("readable", Formats::readable(Formats::extension(file)));
    object.insert("has_cover", !meta.cover.isNull());
    if (command.json()) {
        printJson(object);
    } else {
        print(stdout, "title:\t" + meta.title);
        print(stdout, "authors:\t" + meta.authorText());
        print(stdout, "format:\t" + object.value("format").toString());
    }
    return 0;
}

int toc(const QStringList &arguments) {
    Command command("toc", "List a book's contents: the section each entry starts in, and its title.");
    command.parser.addPositionalArgument("book", "A book's number in the library, or a book file.");
    if (!command.parse(arguments, 1, 1))
        return 1;
    QString error;
    const auto document = openTarget(command, command.argument(0), &error);
    if (!document)
        return fail(error);
    QJsonArray entries;
    for (const TocEntry &entry : document->toc()) {
        entries << QJsonObject{{"section", entry.section}, {"title", entry.title}, {"level", entry.level}};
        if (!command.json())
            print(stdout, QString("%1\t%2%3").arg(entry.section).arg(QString(entry.level * 2, ' '), entry.title));
    }
    if (command.json())
        printJson(QJsonObject{{"sections", document->count()}, {"contents", entries}});
    return 0;
}

int text(const QStringList &arguments) {
    Command command("text", "Print a book as plain text: all of it, or one section (a chapter, or a page of a PDF).");
    command.parser.addPositionalArgument("book", "A book's number in the library, or a book file.");
    command.parser.addOption({"section", "Only this section, counted from 0 as omabook toc does.", "number"});
    if (!command.parse(arguments, 1, 1))
        return 1;
    QString error;
    const auto document = openTarget(command, command.argument(0), &error);
    if (!document)
        return fail(error);
    int from = 0, to = document->count() - 1;
    if (command.parser.isSet("section")) {
        const auto section = Numbers::whole(command.parser.value("section"), 0, document->count() - 1);
        if (!section)
            return fail(QString("This book has sections 0 to %1.").arg(document->count() - 1));
        from = to = static_cast<int>(*section);
    }
    QJsonArray sections;
    for (int section = from; section <= to; ++section) {
        const QString words = document->text(section);
        if (command.json())
            sections << QJsonObject{{"section", section}, {"text", words}};
        else if (!words.isEmpty())
            print(stdout, words + "\n");
    }
    if (command.json())
        printJson(sections);
    return 0;
}

int lookup(const QStringList &arguments) {
    Command command("lookup", "Look a book's details up with every metadata plugin that is on (see omabook plugins), and "
                              "optionally have the AI judge what they found. Each candidate says what it would add and change.");
    command.parser.addPositionalArgument("id", "The book's number.");
    command.parser.addOption({"ai", "Also ask the AI, which reads the book's opening pages and weighs the other results."});
    command.parser.addOption({"apply", "Take the details (and cover) of this candidate: its number in the last listing (made "
                                       "with the same --ai or not), or its id from that listing.", "number|id"});
    if (!command.parse(arguments, 1, 1))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const qint64 id = command.argument(0).toLongLong();
    const auto book = library->book(id);
    if (!book)
        return fail("There is no book " + command.argument(0) + " in the library.");
    const bool ai = command.parser.isSet("ai");
    const QString flags = QString("omabook lookup %1%2").arg(id).arg(ai ? " --ai" : "");

    // A number means what the last listing showed, never a fresh look-up
    // that may come out in another order; an id names one candidate whatever
    // its place.
    const auto take = [&](const MetaCandidate &candidate) {
        if (!BookOps::applyCandidate(*library, id, candidate, &error))
            return fail(error);
        BookOps::forgetListing(*library, id);
        if (command.json())
            printJson(library->book(id)->toJson());
        else
            print(stdout, QString("%1 took the details of %2: %3").arg(id).arg(candidate.source, candidate.meta.title));
        return 0;
    };
    const auto listed = BookOps::lastListing(*library, id);
    const QString wanted = command.parser.value("apply").trimmed();
    bool isNumber = false;
    const int number = wanted.toInt(&isNumber);
    if (command.parser.isSet("apply") && isNumber && listed) {
        if (listed->ai != ai) {
            const QString theirs = number >= 1 && number <= listed->ids.size()
                                       ? QString(" There it was %1 (%2); take it with --apply %3.")
                                             .arg(listed->candidates.at(number - 1).source, listed->candidates.at(number - 1).meta.title,
                                                  listed->ids.at(number - 1))
                                       : QString();
            return fail(QString("The last listing of book %1 was made %2 --ai, so its numbers aren't this look-up's.%3 Or "
                                "list them again with: %4")
                            .arg(id).arg(listed->ai ? "with" : "without", theirs, flags));
        }
        if (number < 1 || number > listed->candidates.size())
            return fail(QString("The last listing of book %1 has %2 candidates.").arg(id).arg(listed->candidates.size()));
        return take(listed->candidates.at(number - 1));
    }
    if (command.parser.isSet("apply") && !isNumber && listed && listed->ids.contains(wanted))
        return take(listed->candidates.at(listed->ids.indexOf(wanted)));

    // The look-up itself is shared with the MCP server; see bookops.h.
    const BookOps::LookUp found = BookOps::lookUp(*library, *book, ai);
    for (const QString &problem : found.problems)
        print(stderr, problem);
    if (!found.error.isEmpty())
        return fail(found.error);
    const QList<MetaCandidate> &candidates = found.candidates;

    if (command.parser.isSet("apply")) {
        if (isNumber) {
            // Nothing was listed lately: the number counts in this look-up,
            // which is said, so it is no surprise.
            if (number < 1 || number > candidates.size())
                return fail(QString("There are %1 candidates.").arg(candidates.size()));
            print(stderr, QString("Nothing was listed for book %1 in the last hour, so %2 is counted in a new look-up.")
                              .arg(id).arg(number));
            return take(candidates.at(number - 1));
        }
        const auto same = std::ranges::find(candidates, wanted, &BookOps::candidateId);
        if (same == candidates.end())
            return fail(QString("No candidate is called %1 now: the sources may answer differently since. List them again "
                                "with: %2").arg(wanted, flags));
        return take(*same);
    }
    BookOps::rememberListing(*library, id, ai, candidates);
    QJsonArray array;
    for (int i = 0; i < candidates.size(); ++i) {
        const MetaCandidate &candidate = candidates.at(i);
        QJsonObject object = BookOps::candidateJson(*book, candidate);
        object.insert("candidate", i + 1);
        object.insert("candidate_id", BookOps::candidateId(candidate));
        array << object;
        if (!command.json()) {
            QStringList changed;
            for (const QJsonValue &change : object.value("changes").toArray())
                changed << change["field"].toString();
            const QStringList adds = object.value("adds").toVariant().toStringList();
            print(stdout, QString("%1\t%2\t%3\t%4\t%5").arg(i + 1).arg(candidate.source, candidate.meta.title,
                                                                    candidate.meta.authorText(), candidate.meta.published));
            print(stdout, "\tid: " + BookOps::candidateId(candidate));
            if (!adds.isEmpty()) print(stdout, "\tadds: " + adds.join(", "));
            if (!changed.isEmpty()) print(stdout, "\tchanges: " + changed.join(", "));
            if (!candidate.why.isEmpty()) print(stdout, "\twhy: " + candidate.why);
        }
    }
    if (command.json())
        printJson(array);
    return candidates.isEmpty() ? 1 : 0;
}

int plugins(const QStringList &arguments) {
    Command command("plugins", "List what Omabook is made of and what has been added to it: built in, Calibre's own "
                               "metadata plugins (run inside Calibre), and programs in the plugin folder. Or turn a "
                               "metadata plugin on or off.");
    command.parser.addPositionalArgument("action", "Nothing to list, or enable or disable.", "[enable|disable]");
    command.parser.addPositionalArgument("id", "The plugin: its id as the list prints it, or its name.", "[id]");
    if (!command.parse(arguments, 0, 2))
        return 1;
    Settings settings;
    Plugins registry(&settings);
    const QString action = command.argument(0);
    if (action == "enable" || action == "disable") {
        QStringList could;
        const QString id = registry.idFor(command.argument(1), &could);
        if (id.isEmpty())
            return fail(could.size() > 1 ? "\"" + command.argument(1) + "\" could be any of: " + could.join(", ") + "; give its id."
                        : "There is no plugin " + command.argument(1) + " that can be turned on or off."
                              + (could.isEmpty() ? QString() : " Did you mean " + could.join(", ") + "?"));
        if (!registry.setEnabled(id, action == "enable"))
            return fail("There is no plugin " + command.argument(1) + " that can be turned on or off.");
    } else if (!action.isEmpty()) {
        return fail("Use omabook plugins, omabook plugins enable <id> or omabook plugins disable <id>.");
    }
    QJsonArray array;
    for (const PluginInfo &plugin : registry.list()) {
        array << QJsonObject{{"id", plugin.id}, {"name", plugin.name}, {"kind", plugin.kind}, {"origin", plugin.origin},
                             {"version", plugin.version}, {"author", plugin.author}, {"description", plugin.description},
                             {"fields", QJsonArray::fromStringList(plugin.fields)}, {"enabled", plugin.enabled},
                             {"switchable", plugin.switchable}, {"problem", plugin.problem}};
        if (!command.json())
            print(stdout, QString("%1\t%2\t%3\t%4%5").arg(plugin.enabled && plugin.problem.isEmpty() ? "on" : "off", plugin.kind,
                                                          plugin.id, plugin.fields.join(","),
                                                          plugin.problem.isEmpty() ? QString() : "\t(" + plugin.problem + ")"));
    }
    if (command.json())
        printJson(QJsonObject{{"folder", registry.folder()}, {"plugins", array}});
    return 0;
}

// Asks the model one thing and waits for the whole answer.
// What the Calibre plugin found on this computer, and importing from it.
int calibre(const QStringList &arguments) {
    Command command("calibre", "What the Calibre plugin found on this computer. `libraries` lists Calibre's libraries with "
                               "their book counts and whether each was imported; `plugins` lists Calibre's own plugins, "
                               "of every kind, saying which Omabook can use; `import` brings a library in (again, to "
                               "bring the reading up to date): the one at <path>, every one not imported yet, or with "
                               "--all every one found. Switch the whole plugin with omabook plugins enable|disable calibre.");
    command.parser.addPositionalArgument("what", "libraries, plugins or import.", "<libraries|plugins|import>");
    command.parser.addPositionalArgument("path", "import: the Calibre library to import.", "[path]");
    command.parser.addOption({"all", "import: every Calibre library found, also those imported or set aside before."});
    command.parser.addOption({"refresh", "plugins: ask Calibre again instead of using what was found last time."});
    if (!command.parse(arguments, 1, 2))
        return 1;
    Settings settings;
    const CalibrePlugin plugin(&settings);
    const QString what = command.argument(0);
    if (what != "libraries" && what != "plugins" && what != "import")
        return fail("Use omabook calibre libraries, omabook calibre plugins or omabook calibre import [<path>|--all].");
    // Off, nothing of Calibre's is looked at. Merely not found, the lists are empty.
    if (plugin.switchedOff())
        return fail("The Calibre plugin is switched off. Switch it on with: omabook plugins enable calibre");

    if (what == "libraries") {
        QJsonArray array;
        for (const CalibreLibrary &library : plugin.libraries()) {
            array << library.toJson();
            if (!command.json())
                print(stdout, QString("%1\t%2\t%3").arg(library.books < 0 ? "?" : QString::number(library.books),
                                                         library.answer.isEmpty() ? "new" : library.answer, library.path));
        }
        if (command.json())
            printJson(array);
        return 0;
    }
    if (what == "plugins") {
        QJsonArray array;
        for (const CalibreAddOn &addOn : plugin.addOns(command.parser.isSet("refresh"))) {
            array << addOn.toJson();
            if (!command.json())
                print(stdout, QString("%1\t%2\t%3\t%4").arg(addOn.usable ? "usable" : "listed",
                                                             addOn.type.isEmpty() ? "Unknown" : addOn.type, addOn.name, addOn.note));
        }
        if (command.json())
            printJson(array);
        return 0;
    }

    QStringList folders;
    if (!command.argument(1).isEmpty()) {
        folders << QDir(command.argument(1)).absolutePath();
        if (!plugin.reads(folders.first()))
            return fail("There is no Calibre library at " + folders.first() + ".");
    } else {
        for (const CalibreLibrary &library : command.parser.isSet("all") ? plugin.libraries() : plugin.librariesToOffer(true))
            folders << library.path;
        if (folders.isEmpty())
            return fail(plugin.libraries().isEmpty() ? "No Calibre library was found on this computer."
                                                     : "Every Calibre library found was already imported or set aside; "
                                                       "name one, or use --all.");
    }
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const ImportSummary summary = importBooks(*library, folders, {}, plugin.importOptions());
    if (command.libraryInUse(settings))
        for (const QString &folder : std::as_const(summary.calibreLibraries))
            settings.setCalibreLibrary(folder, "imported");
    if (command.json())
        printJson(summary.calibreJson());
    else
        print(stdout, summary.calibreLine());
    for (const QString &reason : summary.reasons())
        print(stderr, reason);
    return summary.failed() ? 1 : 0;
}

// The model's whole answer, or why there is none.
[[nodiscard]] std::expected<QString, QString> askAi(const AiRequest &request, bool show = false) {
    QString answer, error, note;
    const bool ok = BookOps::ask(request, &answer, &error, [show](const QString &piece) {
        if (show) {
            fputs(qPrintable(piece), stdout);
            fflush(stdout);
        }
    }, &note);
    // A model that was refused, and which one answered instead.
    if (!note.isEmpty())
        print(stderr, note);
    if (!ok)
        return std::unexpected(error);
    return answer;
}

int notes(const QStringList &arguments) {
    Command command("notes", "Show the notes the AI companion keeps on a book: its catch-up (recap) and who is who (cast), "
                             "each as far as the book was read when it was written.");
    command.parser.addPositionalArgument("id", "The book's number.");
    command.parser.addOption({"kind", "Only this kind: recap or cast.", "kind"});
    if (!command.parse(arguments, 1, 1))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const qint64 id = command.argument(0).toLongLong();
    if (!library->book(id))
        return fail("There is no book " + command.argument(0) + " in the library.");
    QJsonObject object;
    for (const QString &kind : command.parser.isSet("kind") ? QStringList{command.parser.value("kind")} : QStringList{"recap", "cast"}) {
        const Recap kept = library->notes(id, kind);
        if (kept.text.isEmpty())
            continue;
        object.insert(kind, kept.place.toJson({{"text", kept.text},
                                               {"updated", QDateTime::fromSecsSinceEpoch(kept.updated).toString(Qt::ISODate)}}));
        if (!command.json())
            print(stdout, kind + ":\n" + kept.text + "\n");
    }
    if (command.json())
        printJson(object);
    else if (object.isEmpty())
        print(stdout, "No notes are kept on this book yet.");
    return 0;
}

int tidy(const QStringList &arguments) {
    Command command("tidy", "Look the whole library over and list what would tidy it: the same book twice, an author "
                            "spelled several ways and, with --ai, books that belong to a series. Nothing changes without --apply.");
    command.parser.addOption({"ai", "Also ask the AI which books belong to a series."});
    command.parser.addOption({"apply", "Make every change listed, except duplicates marked unsure (pick those by number)."});
    command.parser.addOption({"pick", "Make only these changes, by their numbers in the list: 1,3,4.", "numbers"});
    if (!command.parse(arguments, 0, 0))
        return 1;
    const auto picked = Numbers::list(command.parser.value("pick"));
    if (!picked)
        return fail("--pick takes the numbers in the list, such as 1,3, not \"" + command.parser.value("pick") + "\".");
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const QList<Book> books = library->books();
    QList<Proposal> series;
    if (command.parser.isSet("ai")) {
        const auto answer = askAi(Curation::seriesRequest(books));
        if (!answer)
            return fail(answer.error());
        series = Curation::seriesFrom(*answer, books);
    }
    QJsonArray array;
    int applied = 0, unsure = 0;
    // In the order they can be applied in: merges last (see bookops.h).
    const QList<Proposal> proposals = BookOps::tidy(books, series);
    int number = 0;
    for (const Proposal &proposal : proposals) {
        const auto book = library->book(proposal.bookId);
        if (!book)
            continue;
        ++number;
        QString line = QString("%1\t%2\t%3\t%4\t").arg(number).arg(proposal.kind).arg(proposal.bookId).arg(book->meta.title);
        if (proposal.kind == "duplicate")
            line += QString("book %1 folds into it").arg(proposal.otherId)
                    + (proposal.unsure ? "\tunsure: " + proposal.why : QString());
        else if (proposal.kind == "author")
            line += book->meta.authorText() + " -> " + proposal.meta.authorText();
        else
            line += QString("%1 #%2").arg(proposal.meta.series).arg(proposal.meta.seriesIndex);
        QJsonObject described = BookOps::proposalJson(*book, proposal);
        described.insert("number", number);
        array << described;
        if (!command.json())
            print(stdout, line);
        // An unsure duplicate is only merged when it is picked by its number.
        if (!command.parser.isSet("pick") && command.parser.isSet("apply") && proposal.unsure) {
            ++unsure;
            continue;
        }
        if (command.parser.isSet("pick") ? picked->contains(number) : command.parser.isSet("apply")) {
            if (Curation::apply(*library, proposal, &error))
                ++applied;
            else
                print(stderr, error);
        }
    }
    if (command.json())
        printJson(QJsonObject{{"proposals", array}, {"applied", applied}, {"unsure_left", unsure}});
    else if (command.parser.isSet("apply") || command.parser.isSet("pick"))
        print(stdout, QString("%1 changes made").arg(applied)
                          + (unsure > 0 ? QString("; %1 unsure duplicates left alone: --pick them by number").arg(unsure) : QString()));
    return 0;
}

// Waits for a reading site's shelves: what is on them, or why they couldn't be read.
[[nodiscard]] std::expected<QList<SiteEntry>, QString> fetchShelves(ReadingSite &site) {
    QEventLoop loop;
    std::expected<QList<SiteEntry>, QString> shelves = std::unexpected(QString());
    QObject::connect(&site, &ReadingSite::fetched, &loop, [&shelves, &loop](const QList<SiteEntry> &fetched) {
        shelves = fetched;
        loop.quit();
    });
    QObject::connect(&site, &ReadingSite::failed, &loop, [&shelves, &loop](const QString &problem) {
        shelves = std::unexpected(problem);
        loop.quit();
    });
    site.fetchShelves();
    loop.exec();
    return shelves;
}

// `omabook goodreads ...` and `omabook openlibrary ...`: one command per
// reading site, all with the same actions.
int readingSite(const QStringList &arguments) {
    const QString scheme = arguments.value(1);
    const bool goodreads = scheme == "goodreads";
    Command command(scheme, goodreads
        ? "Keep the library in step with your Goodreads shelves. Goodreads has no API and nothing can write to it, so: "
          "import reads the file Goodreads lets you download (My Books, Import and export), sync reads the shelves of "
          "a public profile, and export writes Omabook's reading as a file for Goodreads' own import page. Books that "
          "are not in the library are added as paper books, or as books only known of. Nothing is ever removed."
        : "Bring the reading log of a public Open Library account into the library: what you want to read, are reading "
          "and have read. Open Library takes nothing back.");
    command.parser.addPositionalArgument("action", goodreads ? "import <file>, sync, export <file> or shelves (list them, change nothing)."
                                                             : "sync, or shelves (list them, change nothing).",
                                         goodreads ? "<import|sync|export|shelves>" : "<sync|shelves>");
    command.parser.addPositionalArgument("file", "The file to import, or to write.", "[file]");
    command.parser.addOption({"user", goodreads ? "The Goodreads user id, profile address or shelf feed address, instead of the one in the settings."
                                                : "The Open Library user name, instead of the one in the settings.", "user"});
    command.parser.addOption({"no-new", "Only update books that are already in the library; add none."});
    command.parser.addOption({"no-covers", "Don't fetch covers for the books that have none."});
    if (!command.parse(arguments, 1, 2))
        return 1;
    const QString action = command.argument(0), file = command.argument(1);
    const bool needsFile = action == "import" || action == "export";
    if (!QStringList{"import", "sync", "export", "shelves"}.contains(action) || needsFile == file.isEmpty())
        return fail(QString("Use omabook %1 %2.").arg(scheme, goodreads ? "import <file>, sync, export <file> or shelves" : "sync or shelves"));

    Settings settings;
    QNetworkAccessManager network;
    const bool online = action == "sync" || action == "shelves";
    if (goodreads && online && !settings.pluginEnabled("builtin:Goodreads", true))
        return fail("The Goodreads plugin is off, so nothing is asked of goodreads.com. Turn it on with: omabook plugins enable builtin:Goodreads");
    // Whoever the settings name, with the key that was kept for them when
    // they connected (omabook sites connect); somebody named here is asked
    // for as anybody would be.
    const bool other = command.parser.isSet("user");
    const std::unique_ptr<ReadingSite> site(ReadingSites::make(
        scheme, other ? command.parser.value("user") : settings.siteIdentity(scheme),
        !other && settings.site(scheme).value("secret").toBool() ? Secrets::shared().get(scheme) : QString(), &network, nullptr));

    QString error;
    if (action == "shelves") {
        const auto entries = fetchShelves(*site);
        if (!entries)
            return fail(entries.error());
        QJsonArray array;
        for (const SiteEntry &entry : *entries) {
            array << entry.toJson();
            if (!command.json())
                print(stdout, QString("%1\t%2\t%3\t%4").arg(entry.shelf, entry.rating > 0 ? QString::number(entry.rating) : QString("-"),
                                                            entry.title, entry.authors.join(" & ")));
        }
        if (command.json())
            printJson(array);
        return 0;
    }

    const auto library = command.library(&error);
    if (!library)
        return fail(error);

    if (action == "export") {
        const QList<Book> books = library->books();
        const QByteArray made = site->writeExport(books);
        if (made.isNull())
            return fail(site->name() + " takes no file back.");
        QSaveFile target(file);
        target.setDirectWriteFallback(false);
        if (!target.open(QIODevice::WriteOnly) || target.write(made) != made.size() || !target.commit())
            return fail("The file can't be written to " + file);
        // Goodreads finds a book by its ISBN first; without one it guesses from the title.
        int written = 0, guessed = 0;
        for (const Book &book : books) {
            if (book.status().isEmpty())
                continue;
            ++written;
            if (ReadingSites::isbn13(book.meta.isbn).isEmpty())
                ++guessed;
        }
        if (command.json())
            printJson(QJsonObject{{"file", QFileInfo(file).absoluteFilePath()}, {"books", written}, {"without_isbn", guessed}});
        else
            print(stdout, QString("%1 books written to %2 (%3 without an ISBN, which Goodreads matches by title). Upload it at "
                                  "https://www.goodreads.com/review/import").arg(written).arg(file).arg(guessed));
        return 0;
    }

    QList<SiteEntry> entries;
    if (action == "import") {
        QFile source(file);
        if (!source.open(QIODevice::ReadOnly))
            return fail("The file can't be read: " + file);
        const ReadingSite::Export read = site->readExport(source.readAll());
        if (!read.error.isEmpty())
            return fail(read.error);
        entries = read.entries;
    } else if (auto fetched = fetchShelves(*site)) {
        entries = std::move(*fetched);
    } else {
        return fail(fetched.error());
    }

    ReadingSites::Options options;
    options.addNew = !command.parser.isSet("no-new");
    const ReadingSites::Summary summary = ReadingSites::merge(*library, entries, site->scheme(), options);

    int covers = 0, coversLeft = 0;
    if (!command.parser.isSet("no-covers") && !summary.covers.isEmpty()) {
        if (!command.json())
            print(stderr, QString("Fetching %1 covers...").arg(summary.covers.size()));
        CoverFetcher fetcher(&network);
        QEventLoop loop;
        QObject::connect(&fetcher, &CoverFetcher::cover, &loop, [&library](qint64 id, const QImage &picture) { library->setCover(id, picture); });
        QObject::connect(&fetcher, &CoverFetcher::done, &loop, [&covers, &coversLeft, &loop](int fetched, int left) {
            covers = fetched;
            coversLeft = left;
            loop.quit();
        });
        fetcher.fetch(summary.covers);
        loop.exec();
    }

    if (command.json()) {
        QJsonObject object = summary.toJson();
        object.insert("entries", entries.size());
        object.insert("covers", covers);
        object.insert("covers_left", coversLeft);
        printJson(object);
    } else {
        for (const ReadingSites::Change &change : summary.changes)
            if (change.what != "unchanged" || !change.note.isEmpty())
                print(stdout, QString("%1\t%2\t%3\t%4").arg(change.what, change.bookId > 0 ? QString::number(change.bookId) : QString("-"),
                                                            change.title, change.note));
        print(stdout, QString("%1 new, %2 updated, %3 unchanged, %4 kept as they are here, %5 unmatched; %6 covers fetched%7")
                          .arg(summary.added).arg(summary.updated).arg(summary.unchanged).arg(summary.kept).arg(summary.unmatched)
                          .arg(covers).arg(coversLeft > 0 ? QString(", %1 left for the next sync").arg(coversLeft) : QString()));
    }
    for (const QString &problem : summary.problems)
        print(stderr, problem);
    return summary.problems.isEmpty() ? 0 : 1;
}

// Reads a line nobody should see typed: the terminal stops echoing for it.
QString readSecretLine() {
    termios before{};
    const bool terminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &before) == 0;
    if (terminal) {
        termios quiet = before;
        quiet.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    QFile input;
    QString line;
    if (input.open(stdin, QIODevice::ReadOnly))
        line = QString::fromUtf8(input.readAll()).trimmed();
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &before);
        fputs("\n", stderr);
    }
    return line;
}

// `omabook sites`: the reading sites as one is connected to them.
int sites(const QStringList &arguments) {
    Command command("sites", "The reading sites (Goodreads, Open Library, Hardcover) and how you are connected to each. "
                             "With no action, lists them. connect takes whatever you have of yourself there (a link to "
                             "your profile, a name, a number) and looks at once; disconnect forgets it and the token, and "
                             "leaves the books; sync brings the shelves in and, where you turned that on, sends what you "
                             "changed here. A token is never an argument: it is read from standard input or from a file, "
                             "and kept in the desktop's keyring, never in config.json.");
    command.parser.addPositionalArgument("action", "Nothing to list, or connect, check, disconnect or sync.", "[connect|check|disconnect|sync]");
    command.parser.addPositionalArgument("site", "goodreads, openlibrary or hardcover; sync takes every connected site when none is given.", "[site]");
    command.parser.addPositionalArgument("identity", "connect: a link to your profile, your user name or id. Hardcover finds it from the token.", "[identity]");
    command.parser.addOption({"token-stdin", "connect: read the token from standard input (Hardcover's token; Open Library's two keys as access:secret)."});
    command.parser.addOption({"token-file", "connect: read the token from this file.", "file"});
    command.parser.addOption({"write", "connect: whether changes made in Omabook also go to the site (yes or no). Off until turned on.", "yes|no"});
    command.parser.addOption({"dry-run", "sync: say what would change here and what would be sent; change nothing."});
    command.parser.addOption({"send", "sync: send the changes to the site the first time, after a dry run showed them."});
    command.parser.addOption({"no-new", "sync: only update books that are already in the library; add none."});
    command.parser.addOption({"no-covers", "sync: don't fetch covers for the books that have none."});
    if (!command.parse(arguments, 0, 3))
        return 1;
    const QString action = command.argument(0), scheme = command.argument(1);
    if (!QStringList{"", "connect", "check", "disconnect", "sync"}.contains(action))
        return fail("Use omabook sites, or sites connect <site> [identity], sites check <site>, sites disconnect <site>, sites sync [site].");
    if (!scheme.isEmpty() && !ReadingSites::schemes().contains(scheme))
        return fail("There is no reading site called " + scheme + "; there are " + ReadingSites::schemes().join(", ") + ".");
    if (scheme.isEmpty() && action != "" && action != "sync")
        return fail("Say which site: " + ReadingSites::schemes().join(", ") + ".");

    Settings settings;
    QNetworkAccessManager network;
    const QString root = command.parser.isSet("library") ? command.parser.value("library") : settings.libraryPath();
    SiteDesk desk(&settings, &network, [root] { return root; });

    auto listed = [&] {
        if (command.json()) {
            printJson(desk.list());
            return;
        }
        for (const QJsonValue &value : desk.list()) {
            const QJsonObject site = value.toObject();
            const QString who = site.value("connected_as").toString().isEmpty() ? site.value("identity").toString()
                                                                                : site.value("connected_as").toString();
            print(stdout, QString("%1\t%2\t%3\t%4").arg(site.value("site").toString(), site.value("state").toString(), who,
                                                        site.contains("why") ? site.value("why").toString()
                                                        : site.contains("last_sync") ? "last sync " + site.value("last_sync").toString().left(10)
                                                        : QString()));
        }
    };
    if (action.isEmpty()) {
        listed();
        return 0;
    }

    // Runs one thing on the desk and waits for it.
    auto wait = [&](const QString &site, const std::function<void()> &run) {
        QJsonObject result;
        QEventLoop loop;
        const auto link = QObject::connect(&desk, &SiteDesk::done, &loop, [&site, &result, &loop](const QString &about, const QJsonObject &answer) {
            if (about == site) {
                result = answer;
                loop.quit();
            }
        }, Qt::QueuedConnection);
        run();
        loop.exec();
        QObject::disconnect(link);
        return result;
    };

    if (action == "disconnect") {
        desk.disconnectSite(scheme);
        if (command.json())
            printJson(desk.lastResult(scheme));
        else
            print(stdout, "Forgot who you are on " + scheme + ", and its token. Your books are as they were.");
        return 0;
    }

    if (action == "connect") {
        QString secret;
        if (command.parser.isSet("token-file")) {
            QFile file(command.parser.value("token-file"));
            if (!file.open(QIODevice::ReadOnly))
                return fail("The token can't be read from " + command.parser.value("token-file"));
            secret = QString::fromUtf8(file.readAll()).trimmed();
        } else if (command.parser.isSet("token-stdin")) {
            if (isatty(STDIN_FILENO))
                print(stderr, "Paste the token and press Enter, then Ctrl+D (it is not shown):");
            secret = readSecretLine();
        }
        if (!secret.isEmpty()) {
            // Said before anything is kept: a token that would be gone when
            // this command ends is no use to the next one.
            QString why;
            if (!Secrets::shared().lasting(&why))
                return fail(why + " Nothing was kept.");
        }
        if (command.parser.isSet("write"))
            desk.setWrite(scheme, command.parser.value("write").toLower() == "yes");
        const QJsonObject result = wait(scheme, [&] { desk.connectSite(scheme, command.argument(2), secret); });
        if (command.json())
            printJson(result);
        else if (result.value("ok").toBool())
            listed();
        return result.value("ok").toBool() ? 0 : fail(result.value("error").toString());
    }

    if (action == "check") {
        const QJsonObject result = wait(scheme, [&] { desk.checkSite(scheme); });
        if (command.json())
            printJson(result);
        else if (result.value("ok").toBool())
            listed();
        return result.value("ok").toBool() ? 0 : fail(result.value("error").toString());
    }

    // sync: one site, or every one that is connected.
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    QStringList schemes;
    for (const QJsonValue &value : desk.list())
        if (scheme.isEmpty() ? value.toObject().value("state").toString() == "connected" : value.toObject().value("site").toString() == scheme)
            schemes << value.toObject().value("site").toString();
    if (schemes.isEmpty())
        return fail("No reading site is connected. Connect one with: omabook sites connect <site> [identity]");
    QList<QPair<qint64, QUrl>> covers;
    QObject::connect(&desk, &SiteDesk::coversWanted, &network, [&covers](const QList<QPair<qint64, QUrl>> &wanted) { covers += wanted; });
    SiteDesk::SyncOptions options;
    options.addNew = !command.parser.isSet("no-new");
    options.dryRun = command.parser.isSet("dry-run");
    options.send = command.parser.isSet("send");
    QJsonArray results;
    bool allWell = true;
    for (const QString &site : std::as_const(schemes)) {
        const QJsonObject result = wait(site, [&] { desk.sync(site, options); });
        results << result;
        allWell = allWell && result.value("ok").toBool() && result.value("send_failed").toArray().isEmpty();
        if (command.json())
            continue;
        if (!result.value("ok").toBool()) {
            print(stderr, site + ": " + result.value("error").toString());
            continue;
        }
        for (const QJsonValue &value : result.value("changes").toArray())
            print(stdout, QString("%1\t%2\t%3\t%4").arg(value.toObject().value("change").toString(),
                                                        value.toObject().contains("id") ? QString::number(value.toObject().value("id").toInteger()) : QString("-"),
                                                        value.toObject().value("title").toString(), value.toObject().value("note").toString()));
        // What goes, or would go, to the site.
        const bool held = result.value("held").toInt() > 0 || options.dryRun;
        for (const QJsonValue &value : result.value("to_send").toArray()) {
            const QJsonObject change = value.toObject();
            QStringList what;
            if (change.contains("status")) what << "status " + change.value("status").toString();
            if (change.contains("rating")) what << QString("%1 stars").arg(change.value("rating").toInt());
            if (change.contains("finished")) what << "finished " + change.value("finished").toString();
            print(stdout, QString("%1\t%2\t%3\t%4").arg(held ? "would send" : "sent", QString::number(change.value("id").toInteger()),
                                                        change.value("title").toString(), what.join(", ")));
        }
        for (const QJsonValue &value : result.value("send_failed").toArray())
            print(stderr, QString("not taken\t%1\t%2").arg(value.toObject().value("title").toString(), value.toObject().value("error").toString()));
        print(stdout, QString("%1: %2%3 new, %4 updated, %5 unchanged, %6 kept as they are here, %7 unmatched")
                          .arg(site, options.dryRun ? "(nothing changed) " : "").arg(result.value("new").toInt()).arg(result.value("updated").toInt())
                          .arg(result.value("unchanged").toInt()).arg(result.value("kept").toInt()).arg(result.value("unmatched").toInt()));
        if (result.value("held").toInt() > 0)
            print(stdout, QString("Nothing was sent to %1 yet: this is the first time. Look the lines marked \"would send\" over, "
                                  "then run: omabook sites sync %1 --send").arg(site));
        for (const QJsonValue &problem : result.value("problems").toArray())
            print(stderr, problem.toString());
    }
    if (!command.parser.isSet("no-covers") && !covers.isEmpty() && !options.dryRun) {
        if (!command.json())
            print(stderr, QString("Fetching %1 covers...").arg(covers.size()));
        CoverFetcher fetcher(&network);
        QEventLoop loop;
        QObject::connect(&fetcher, &CoverFetcher::cover, &loop, [&library](qint64 id, const QImage &picture) { library->setCover(id, picture); });
        QObject::connect(&fetcher, &CoverFetcher::done, &loop, &QEventLoop::quit);
        fetcher.fetch(covers);
        loop.exec();
    }
    if (command.json())
        printJson(scheme.isEmpty() ? QJsonValue(results) : results.first());
    return allWell ? 0 : 1;
}

int suggest(const QStringList &arguments) {
    Command command("suggest", "Have the AI suggest what to read next that you don't have, going by what you read (with "
                               "your stars), are reading and want to read: files, paper books and books only known of "
                               "from a reading site alike. Each suggestion is then looked up on Goodreads and Open "
                               "Library: that it exists, what readers rate it, and whether it is already yours.");
    command.parser.addOption({"no-check", "Don't look the suggestions up."});
    command.parser.addOption({"collection", "Go only by the books of this collection, by its path or name, with the graph of its top-level collection.", "name"});
    if (!command.parse(arguments, 0, 0))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const QList<Book> held = library->books();
    QList<Book> goneBy = held;
    if (command.parser.isSet("collection")) {
        const auto collection = library->collectionNamed(command.parser.value("collection"));
        if (!collection)
            return fail("There is no collection called " + command.parser.value("collection") + ". omabook collections lists them.");
        goneBy = collection->of(held);
    }
    QList<Curation::BookNotes> books;
    for (const Book &book : goneBy)
        books.append({book, library->notes(book.id, "recap").text});
    const auto picture = GraphContext::pictureIn(*library, command.parser.value("collection"), {}, Settings().graph() != "off");
    const auto answer = picture ? askAi(Curation::suggestionRequest(books, *picture)) : std::expected<QString, QString>(std::unexpected(picture.error()));
    if (!answer)
        return fail(answer.error());
    const Curation::Suggestions suggested = Curation::suggestionsFrom(*answer);
    if (suggested.books.isEmpty()) {
        // The model answered in its own words; they are the answer.
        if (command.json())
            printJson(QJsonObject{{"suggestions", QJsonArray()}, {"answer", answer->trimmed()}});
        else
            print(stdout, answer->trimmed());
        return 0;
    }

    Settings settings;
    QNetworkAccessManager network;
    const QList<ReadingSite *> sites = command.parser.isSet("no-check") ? QList<ReadingSite *>()
                                                                        : ReadingSites::all(settings, &network, &network);
    SuggestionCheck check(sites);
    QEventLoop loop;
    QObject::connect(&check, &SuggestionCheck::done, &loop, &QEventLoop::quit, Qt::QueuedConnection);
    check.check(suggested.books, held);
    loop.exec();

    QJsonArray array;
    for (const ReadingSites::Checked &checked : check.results()) {
        array << checked.toJson();
        if (command.json())
            continue;
        QStringList notes;
        if (checked.state == "found" && checked.book.averageRating > 0)
            notes << QString("%1 on %2").arg(QString::number(checked.book.averageRating, 'f', 2), checked.book.site);
        else if (checked.state == "found")
            notes << "on " + checked.book.site;
        else if (checked.state == "missing")
            notes << "not found on any reading site";
        if (checked.bookId > 0)
            notes << (checked.shelf == "to-read" ? QString("already on your to-read list")
                      : checked.shelf.isEmpty() ? QString("already in the library") : "already yours: " + checked.shelf);
        const QJsonObject shown = checked.toJson();
        print(stdout, QString("%1 — %2%3").arg(shown.value("title").toString(), shown.value("author").toString(),
                                               notes.isEmpty() ? QString() : " (" + notes.join("; ") + ")"));
        if (!checked.suggestion.why.isEmpty())
            print(stdout, "    " + checked.suggestion.why);
    }
    if (command.json())
        printJson(QJsonObject{{"suggestions", array}, {"first", suggested.first}});
    else if (!suggested.first.isEmpty())
        print(stdout, "\n" + suggested.first);
    return 0;
}

int change(const QStringList &arguments) {
    Command command("change", "Ask the AI to change how Omabook looks or works (\"make it warmer\", \"bigger covers\", "
                              "\"Space for the next chapter\"). It rewrites keys.json, style.json and the look settings; "
                              "--undo puts back what the last change replaced.");
    command.parser.addPositionalArgument("wish", "What to change, in your own words.", "[wish...]");
    command.parser.addOption({"undo", "Put back what the last change replaced."});
    if (!command.parse(arguments, 0))
        return 1;
    Customization custom;
    Settings settings;
    // The library, for a change that makes a collection; a change to the
    // look needs none.
    QString libraryError;
    const auto library = command.library(&libraryError);
    if (command.parser.isSet("undo")) {
        if (!InterfaceChanges::undo(custom, settings, library.get()))
            return fail("There is no change to undo.");
        print(stdout, "Put back as it was.");
        return 0;
    }
    const QString wish = command.parser.positionalArguments().join(' ');
    if (wish.trimmed().isEmpty())
        return fail("Say what to change, or use --undo.");
    // The colors in force are the chosen theme's with style.json over them.
    Theme theme;
    ThemeChoice choice(&custom, ThemePlaces::here(custom.folder()));
    const Look look(&theme, &choice, &custom);
    const auto answer = askAi(Prompts::change(InterfaceChanges::state(custom, settings, look.colors(), library.get()), wish));
    if (!answer)
        return fail(answer.error());
    const Prompts::InterfaceChange asked = Prompts::parseChange(*answer);
    const QString problem = InterfaceChanges::apply(custom, settings, asked, library.get());
    if (!problem.isEmpty())
        return fail("Nothing was changed: " + problem);
    if (command.json())
        printJson(QJsonObject{{"say", asked.say}, {"keys", asked.hasKeys}, {"style", asked.hasStyle}, {"settings", asked.settings},
                              {"collections", asked.collections}});
    else
        print(stdout, asked.say.isEmpty() ? "Changed." : asked.say);
    return 0;
}

int customize(const QStringList &arguments) {
    Command command("customize", "Show the folder that holds Omabook's interface as files (keys, look, views, panels) and "
                                 "write its guide, AGENTS.md. With a view's name, also write the built-in view there to start from.");
    command.parser.addPositionalArgument("view", "A built-in view to copy into ui/, such as ReaderView.", "[view]");
    if (!command.parse(arguments, 0, 1))
        return 1;
    const Customization custom;
    const QString guide = custom.writeGuide();
    if (guide.isEmpty())
        return fail("The guide can't be written to " + custom.folder());
    QString view, problem;
    if (!command.argument(0).isEmpty()) {
        view = custom.exportView(command.argument(0), &problem);
        if (view.isEmpty())
            return fail(problem);
    }
    if (command.json()) {
        printJson(QJsonObject{{"folder", custom.folder()}, {"guide", guide}, {"view", view},
                              {"views", QJsonArray::fromStringList(Customization::builtInViews())}});
    } else {
        print(stdout, "Omabook's interface lives in " + custom.folder());
        print(stdout, "How to change it: " + guide);
        if (!view.isEmpty())
            print(stdout, "A view to edit: " + view);
    }
    return 0;
}

int catalog(const QStringList &arguments) {
    Command command("catalog", "Write the library, or the books matching a search, as one file: csv for a spreadsheet, "
                               "bibtex for references.");
    command.parser.addPositionalArgument("search", "Words to look for, as omabook list takes them.", "[search...]");
    command.parser.addOption({"format", "csv or bibtex; csv when not given.", "format", "csv"});
    command.parser.addOption({"output", "Write to this file instead of standard output.", "file"});
    if (!command.parse(arguments, 0))
        return 1;
    QString error;
    const auto library = command.library(&error);
    if (!library)
        return fail(error);
    const QList<Book> books = library->books(command.parser.positionalArguments().join(' '));
    const QString format = command.parser.value("format").toLower();
    QByteArray made = Catalog::make(books, format);
    if (made.isNull()) {
        // Not one of Omabook's own: a format a script plugin writes.
        Settings settings;
        CommandScripts scripts(&settings, library->root());
        const QStringList added = scripts.plugins().catalogFormats();
        if (!added.contains(format))
            return fail("There is no catalog format " + format + "; use csv or bibtex"
                        + (added.isEmpty() ? QString() : ", or one a plugin adds: " + added.join(", ")) + ".");
        made = scripts.plugins().makeCatalog(format, books, &error);
        if (made.isNull())
            return fail(error);
    }
    if (!command.parser.isSet("output")) {
        fwrite(made.constData(), 1, made.size(), stdout);
        return 0;
    }
    QSaveFile file(command.parser.value("output"));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(made) != made.size() || !file.commit())
        return fail("The catalog can't be written to " + command.parser.value("output"));
    return 0;
}

int ask(const QStringList &arguments) {
    Command command("ask", "Ask the AI a question about a book, with the book's text to go on.");
    command.parser.addPositionalArgument("book", "A book's number in the library, a book file, or \"all\" to ask about the library as a whole.");
    command.parser.addPositionalArgument("question", "What to ask.", "<question...>");
    command.parser.addOption({"upto", "Send the text only up to and including this section, so later ones stay unread.", "number"});
    command.parser.addOption({"collection", "With all: ask only about the books of this collection, with the graph of its top-level collection.", "name"});
    if (!command.parse(arguments, 2, -1, {Numbers::section("upto")}))
        return 1;
    QString error;
    if (command.argument(0) == "all") {
        // The whole library: what it holds and the notes on what was read.
        const auto library = command.library(&error);
        if (!library)
            return fail(error);
        QList<Book> asked = library->books();
        if (command.parser.isSet("collection")) {
            const auto collection = library->collectionNamed(command.parser.value("collection"));
            if (!collection)
                return fail("There is no collection called " + command.parser.value("collection") + ". omabook collections lists them.");
            asked = collection->of(asked);
        }
        // The knowledge graph's picture of what was read goes along, and the
        // question joins the graph once it is answered; neither with it off.
        const QString question = command.parser.positionalArguments().mid(1).join(' ');
        const bool graph = Settings().graph() != "off";
        const auto picture = GraphContext::pictureIn(*library, command.parser.value("collection"), question, graph);
        const auto answer = picture ? askAi(Curation::libraryQuestion(BookOps::booksWithNotes(*library, asked), question, *picture), !command.json()) : std::expected<QString, QString>(std::unexpected(picture.error()));
        if (!answer)
            return fail(answer.error());
        if (graph)
            GraphContext::noteQuestion(Graph::forTenant(*library, *Tenancy::chosen(*library, command.parser.value("collection"))), 0, std::nullopt, question, *answer);
        if (command.json())
            printJson(QJsonObject{{"answer", answer->trimmed()}});
        else
            print(stdout, "");
        return 0;
    }
    const auto document = openTarget(command, command.argument(0), &error);
    if (!document)
        return fail(error);
    // A book of the library has a knowledge graph: what it holds on the
    // things the question names goes along, up to the same section as the
    // text, and the question joins it once it is answered; and the reader's annotations (#86).
    QString libraryError;
    const auto library = command.library(&libraryError);
    bool inLibrary = false;
    const qint64 id = command.argument(0).toLongLong(&inLibrary);
    const bool graph = inLibrary && library && library->book(id) && Settings().graph() != "off";
    const QString question = command.parser.positionalArguments().mid(1).join(' ');
    const int upTo = command.parser.isSet("upto") ? command.parser.value("upto").toInt() : -1;
    const GraphPlace asked{id, upTo < 0 ? document->count() - 1 : upTo, 1.0};
    bool trimmed = false;
    const AiRequest request = BookOps::bookQuestion(*document, question, upTo, &trimmed,
                                                    graph ? GraphContext::forQuestion(Graph::ofReader(*library, id), id, asked, question) : QString(),
                                                    BookOps::readersMarks(library.get(), inLibrary ? id : 0, upTo));
    if (trimmed)
        print(stderr, "The text is longer than fits in one question, so only its last part was sent.");
    const auto answer = askAi(request, !command.json());
    if (!answer)
        return fail(answer.error());
    if (graph)
        GraphContext::noteQuestion(Graph::ofReader(*library, id), id, asked, question, *answer);
    if (command.json())
        printJson(QJsonObject{{"answer", answer->trimmed()}});
    else
        print(stdout, "");
    return 0;
}

// The skill is laid out as agents' skills are: a short SKILL.md that is
// read every time, and parts in references/ read only when the task needs
// them. The part on writing plugins is made from the table the scripts' own
// `omabook` object is described by, so it says what this build offers.
namespace {
const QStringList skillParts{"library", "graph", "plugins"};

std::optional<QByteArray> skillPart(const QString &part) {
    if (part == "plugins")
        return QByteArray("# Writing a plugin\n\nPart of the Omabook skill (`omabook skill plugins`; `omabook plugin api` prints the same).\n")
               + ScriptApi::guide().section('\n', 2).replace("\n### ", "\n## ").toUtf8();
    QFile bundled(part.isEmpty() ? QString(":/skill.md") : ":/skill/" + part + ".md");
    if (!bundled.open(QIODevice::ReadOnly))
        return std::nullopt;
    return bundled.readAll();
}
} // namespace

int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, one part of it (library, graph, plugins), or all of it with --all; "
                             "install it for your coding agents with: skill install.");
    command.parser.addPositionalArgument("part", "library, graph or plugins; or install, to copy it to ~/.agents/skills/omabook "
                                                 "and link it into ~/.claude/skills.", "[part|install]");
    command.parser.addOption({"all", "The skill and every part, one after the other."});
    if (!command.parse(arguments, 0, 1))
        return 1;
    const QString asked = command.argument(0);
    if (asked != "install") {
        QStringList parts{QString()};
        if (command.parser.isSet("all"))
            parts += skillParts;
        else if (!asked.isEmpty())
            parts = {asked};
        if (!asked.isEmpty() && !skillParts.contains(asked))
            return fail("There is no part " + asked + " of the skill. Its parts are library, graph and plugins.");
        QStringList text;
        for (const QString &part : parts) {
            const auto contents = skillPart(part);
            if (!contents)
                return fail("The skill is missing from this build.");
            text << QString::fromUtf8(*contents).trimmed();
        }
        print(stdout, text.join("\n\n"));
        return 0;
    }
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/omabook");
    if (!QDir().mkpath(directory + "/references"))
        return fail("The skill can't be written to " + directory);
    const QStringList written = QStringList{QString()} + skillParts;
    for (const QString &part : written) {
        const auto contents = skillPart(part);
        if (!contents)
            return fail("The skill is missing from this build.");
        QSaveFile target(part.isEmpty() ? directory + "/SKILL.md" : directory + "/references/" + part + ".md");
        if (!target.open(QIODevice::WriteOnly) || target.write(*contents) != contents->size() || !target.commit())
            return fail("The skill can't be written to " + directory);
    }
    // Claude Code looks in its own folder; a link keeps one copy.
    const QString link = home.filePath(".claude/skills/omabook");
    if (!QFileInfo::exists(link) && !QFileInfo(link).isSymLink()
        && (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/omabook", link)))
        return fail("Installed in " + directory + ", but it can't be linked into ~/.claude/skills.");
    print(stdout, "Installed the Omabook skill in " + directory);
    return 0;
}

// The help the window shows under ?, for a terminal and for agents: the
// same pages from the same text, and the keys as keys.json has them now.
int help(const QStringList &arguments) {
    Command command("help", "Print a page of Omabook's help, the same the window shows under ?. Without a topic, "
                            "list the commands and the topics. The topic `keys` lists every key as it is bound now, "
                            "and `new` what this version brought.");
    command.parser.addPositionalArgument("topic", "A topic, such as reading, ai, library or keys.", "[topic]");
    if (!command.parse(arguments, 0, 1))
        return 1;
    Customization custom;
    const Manual manual(nullptr, &custom);
    const QString topic = command.argument(0).toLower();
    // The keys are the window's: with what the script plugins that are
    // switched on register, under each plugin's name.
    Settings settings;
    std::optional<ScriptPlugins> scripts;
    if (topic == "keys") {
        ScriptServices services;
        services.settings = &settings;
        scripts.emplace(services, QString(), nullptr, false);
        custom.setPlugins(scripts->commands(), {});
    }
    if (topic.isEmpty()) {
        if (command.json())
            printJson(manual.overviewJson());
        else
            print(stdout, "Omabook " OMABOOK_VERSION ": an ebook library and reader.\n\nUsage: omabook <command> [options]\n\n"
                              + cliSummary() + "\n\n" + manual.overviewText());
        return 0;
    }
    if (topic == "keys") {
        if (command.json())
            printJson(manual.keysJson());
        else
            print(stdout, manual.keysText());
        return 0;
    }
    if (topic == "new") {
        QJsonArray features;
        QString text = "New in Omabook " + manual.version() + "\n";
        for (const QVariant &value : manual.news(true)) {
            const QVariantMap feature = value.toMap();
            const QString words = feature.value("text").toString();
            features << QJsonObject{{"id", feature.value("id").toString()}, {"title", feature.value("title").toString()},
                                    {"text", words}, {"command", feature.value("command").toString()},
                                    {"key", feature.value("key").toString()}};
            text += "\n" + feature.value("title").toString() + "\n  " + words + "\n";
        }
        if (command.json())
            printJson(QJsonObject{{"version", manual.version()}, {"features", features}});
        else
            print(stdout, text.trimmed());
        return 0;
    }
    if (manual.topicText(topic).isEmpty())
        return fail("There is no help on \"" + topic + "\". " + manual.overviewText());
    if (command.json())
        printJson(manual.topicJson(topic));
    else
        print(stdout, manual.topicText(topic));
    return 0;
}

// Every command by its name. What is run and what counts as a command are
// both read from here, so the two can't come apart.
struct CliCommand {
    QLatin1StringView name;
    int (*run)(const QStringList &arguments);
};

constexpr std::array cliCommands{
    CliCommand{"list"_L1, list},
    CliCommand{"search"_L1, search},
    CliCommand{"show"_L1, show},
    CliCommand{"add"_L1, add},
    CliCommand{"folio"_L1, Folio::run},
    CliCommand{"bookmarks"_L1, runBookmarksCli},
    CliCommand{"annotations"_L1, runAnnotationsCli},
    CliCommand{"logs"_L1, runLogsCli},
    CliCommand{"notes"_L1, notes},
    CliCommand{"libraries"_L1, runLibrariesCli},
    CliCommand{"remove"_L1, remove},
    CliCommand{"set"_L1, set},
    CliCommand{"info"_L1, info},
    CliCommand{"toc"_L1, toc},
    CliCommand{"text"_L1, text},
    CliCommand{"find"_L1, runFindCli},
    CliCommand{"meaning"_L1, runMeaningCli},
    CliCommand{"lookup"_L1, lookup},
    CliCommand{"convert"_L1, runConvertCli},
    CliCommand{"send"_L1, runSendCli},
    CliCommand{"export"_L1, runExportCli},
    CliCommand{"embed"_L1, runEmbedCli},
    CliCommand{"ask"_L1, ask},
    CliCommand{"ai"_L1, runAiCli},
    CliCommand{"graph"_L1, runGraphCliWithChronology},
    CliCommand{"chronology"_L1, runChronologyCli},
    CliCommand{"progress"_L1, runProgressCli},
    CliCommand{"stats"_L1, runStatsCli},
    CliCommand{"format"_L1, format},
    CliCommand{"sync"_L1, runSyncCli},
    CliCommand{"koreader"_L1, runKoreaderCli},
    CliCommand{"serve"_L1, runServeCli},
    CliCommand{"opds"_L1, runOpdsCli},
    CliCommand{"manifest"_L1, runManifestCli},
    CliCommand{"identity"_L1, runIdentityCli},
    CliCommand{"hub"_L1, runHubCli},
    CliCommand{"snapshot"_L1, runSnapshotCli},
    CliCommand{"spaces"_L1, runSpacesCli},
    CliCommand{"plugins"_L1, plugins},
    CliCommand{"plugin"_L1, runPluginCli},
    CliCommand{"calibre"_L1, calibre},
    CliCommand{"catalog"_L1, catalog},
    CliCommand{"collections"_L1, Collections::run},
    CliCommand{"customize"_L1, customize},
    CliCommand{"theme"_L1, runThemeCli},
    CliCommand{"tidy"_L1, tidy},
    CliCommand{"suggest"_L1, suggest},
    CliCommand{"goodreads"_L1, readingSite},
    CliCommand{"openlibrary"_L1, readingSite},
    CliCommand{"sites"_L1, sites},
    CliCommand{"change"_L1, change},
    CliCommand{"skill"_L1, skill},
    CliCommand{"help"_L1, help},
    CliCommand{"mcp"_L1, Mcp::run},
};

// The command of that name, or the end of the table.
[[nodiscard]] auto cliCommand(const QString &word) {
    return std::ranges::find_if(cliCommands, [&word](const CliCommand &command) { return word == command.name; });
}

} // namespace

bool isCliCommand(const QString &word) { return cliCommand(word) != cliCommands.end(); }

QStringList commandFirst(const QStringList &arguments) {
    static const QStringList valued{"--library", "--model"};
    qsizetype at = 1;
    while (at < arguments.size() && arguments.at(at).startsWith('-')) {
        const QString &word = arguments.at(at);
        at += valued.contains(word) ? 2 : 1;
    }
    if (at == 1 || at >= arguments.size() || (!isCliCommand(arguments.at(at)) && arguments.at(at) != "open"))
        return arguments;
    // Moved as they are: an option the command doesn't know is then the
    // command's to name.
    return arguments.first(1) + QStringList{arguments.at(at)} + arguments.mid(1, at - 1) + arguments.mid(at + 1);
}

int runCli(const QStringList &given) {
    // --model <name> is taken by every command: the model for whatever the command asks the AI, this once.
    // "main" and "quick" name the two the settings hold; anything else is a model's own name.
    QStringList arguments = given;
    QString model;
    for (qsizetype i = 2; i < arguments.size();) {
        if (arguments.at(i) == "--model" && i + 1 < arguments.size()) {
            model = arguments.at(i + 1);
            arguments.remove(i, 2);
        } else if (arguments.at(i).startsWith("--model=")) {
            model = arguments.at(i).mid(8);
            arguments.removeAt(i);
        } else {
            ++i;
        }
    }
    const Ai::ModelOverride forThisRun(model);
    // Book formats that script plugins add are read by every command, the MCP server and the file manager's
    // helper; the plugins are loaded only when a format Omabook doesn't read itself is asked about.
    ScriptFormats::loadOnDemand();
    const auto unloadScripts = qScopeGuard([] { ScriptFormats::unload(); });
    if (const auto command = cliCommand(arguments.value(1)); command != cliCommands.end()) {
        snapshotBeforeRisky(arguments); // a moment snapshot first, of a sync, an import, a tidy (#181)
        const int code = command->run(arguments);
        // What a command changed in a library shows in its view in the file manager at once; a library nothing
        // changed in costs a look at its change counter. Folio's own commands keep the views themselves.
        if (command->name != "folio"_L1)
            Folio::follow(Settings());
        return code;
    }
    print(stdout, "Omabook " OMABOOK_VERSION ": an ebook library and reader.\n\nUsage: omabook <command> [options]\n\n" + cliSummary());
    return 0;
}
