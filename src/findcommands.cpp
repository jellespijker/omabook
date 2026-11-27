#include "findcommands.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <expected>
#include <memory>
#include <print>
#include <tuple>

#include "airegistry.h"
#include "bookops.h"
#include "document.h"
#include "embeddings.h"
#include "library.h"
#include "meaningsearch.h"
#include "numberoptions.h"
#include "settings.h"
#include "tenancy.h"

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

void printJson(const QJsonDocument &document) { printLine(stdout, QString::fromUtf8(document.toJson(QJsonDocument::Indented)).trimmed()); }

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
[[nodiscard]] bool parsed(QCommandLineParser &parser, const QStringList &arguments, const QString &name, qsizetype atLeast) {
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook "_s + name;
    parser.process(rest);
    if (parser.positionalArguments().size() < atLeast) {
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

// A book of the library by its number.
[[nodiscard]] std::expected<Book, QString> bookNamed(const Library &library, const QString &word) {
    bool number = false;
    const qint64 id = word.toLongLong(&number);
    if (const auto book = number ? library.book(id) : std::nullopt)
        return *book;
    return std::unexpected(u"There is no book %1 in the library."_s.arg(word));
}

// Says where a book's text goes before it is sent: a hosted server only as the settings allow.
void sayWhere(const Embeddings::Route &route, const Book &book) {
    printLine(stderr, u"Indexing %1 by its meaning with %2 on %3 (%4)."_s.arg(book.meta.title, route.model, route.server.name,
                                                                              route.server.host()));
    if (route.hosted)
        printLine(stderr, u"%1 is someone else's computer: the book's text goes there, as the settings allow."_s.arg(route.server.host()));
}

int indexBooks(const Library &library, const Embeddings::Route &route, const QList<qint64> &books, bool json) {
    QJsonArray done;
    int failed = 0;
    for (const qint64 bookId : books) {
        const auto book = library.book(bookId);
        if (!book)
            continue;
        sayWhere(route, *book);
        QElapsedTimer took;
        took.start();
        const auto made = MeaningSearch::index(library, route, bookId);
        if (!made) {
            ++failed;
            printLine(stderr, u"Book %1: %2"_s.arg(bookId).arg(made.error().text()));
            continue;
        }
        done.append(QJsonObject{{"book", bookId}, {"stretches", *made}, {"seconds", static_cast<double>(took.elapsed()) / 1000}});
        if (!json)
            printLine(stdout, u"%1\t%2\t%3 stretches in %4 s"_s.arg(bookId)
                                  .arg(book->meta.title)
                                  .arg(*made)
                                  .arg(static_cast<double>(took.elapsed()) / 1000.0, 0, 'f', 1));
    }
    if (json)
        printJson(QJsonDocument(done));
    return failed > 0 ? 1 : 0;
}

[[nodiscard]] QString freshnessName(MeaningSearch::Freshness freshness) {
    switch (freshness) {
    case MeaningSearch::Freshness::Fresh:
        return u"indexed"_s;
    case MeaningSearch::Freshness::Stale:
        return u"out of date"_s;
    case MeaningSearch::Freshness::None:
        break;
    }
    return u"not yet"_s;
}

int status(const Library &library, const Settings &settings, bool json) {
    const auto route = AiRegistry::embeddings(settings);
    const QString model =
        route ? route->model : settings.meaning().value("model"_L1).toString(QString::fromLatin1(Embeddings::defaultModel));
    QJsonArray books;
    for (const qint64 bookId : MeaningIndex::books(library)) {
        const auto state = MeaningIndex::state(library, bookId);
        const auto book = library.book(bookId);
        if (!state || !book)
            continue;
        books.append(QJsonObject{{"book", bookId},
                                 {"title", book->meta.title},
                                 {"state", freshnessName(MeaningSearch::freshness(library, bookId, model))},
                                 {"stretches", state->stretches},
                                 {"model", state->model}});
    }
    if (json) {
        printJson(QJsonDocument(QJsonObject{{"server", route ? route->server.baseUrl : QString()},
                                            {"model", model},
                                            {"hosted", route && route->hosted},
                                            {"problem", route ? QString() : route.error().text()},
                                            {"all", settings.meaningAll()},
                                            {"books", books}}));
        return 0;
    }
    printLine(stdout, route ? u"Embeddings: %1 on %2 (%3)%4"_s.arg(model, route->server.name, route->server.baseUrl,
                                                                   route->hosted ? u", someone else's computer"_s : QString())
                            : u"Embeddings: %1"_s.arg(route.error().text()));
    printLine(stdout, settings.meaningAll() ? u"Every book is indexed."_s : u"Only the books asked for are indexed."_s);
    for (const auto &listed : std::as_const(books)) {
        const QJsonObject book = listed.toObject();
        printLine(stdout, u"%1\t%2\t%3\t%4 stretches"_s.arg(book.value("book"_L1).toInteger())
                              .arg(book.value("title"_L1).toString(), book.value("state"_L1).toString())
                              .arg(book.value("stretches"_L1).toInt()));
    }
    return 0;
}

// Changes what the settings say of the index, from the options given.
int use(const QCommandLineParser &parser) {
    Settings settings;
    for (const char *key : {"url", "model"})
        if (parser.isSet(QLatin1StringView(key)))
            settings.setMeaningChoice(QLatin1StringView(key), parser.value(QLatin1StringView(key)).trimmed());
    for (const char *key : {"hosted", "all"})
        if (parser.isSet(QLatin1StringView(key))) {
            const QString said = parser.value(QLatin1StringView(key)).toLower();
            if (said != "on"_L1 && said != "off"_L1)
                return failLine(u"--%1 takes on or off."_s.arg(QLatin1StringView(key)));
            settings.setMeaningChoice(QLatin1StringView(key), said == "on"_L1);
        }
    printJson(QJsonDocument(settings.meaning()));
    return 0;
}

int search(const Library &library, const QCommandLineParser &parser, const QStringList &words) {
    const auto route = AiRegistry::embeddings(Settings());
    if (!route)
        return failLine(route.error().text());
    MeaningSearch::Question question{.words = words.mid(1).join(u' '), .books = {}, .whole = parser.isSet("whole"), .limit = 10};
    if (const auto why = Numbers::problem(parser, {{.name = u"limit"_s, .least = 1}}))
        return failLine(*why);
    if (parser.isSet("limit"))
        question.limit = parser.value("limit").toInt();
    if (words.first() == "all"_L1) {
        const auto tenant = Tenancy::chosen(library, parser.value("collection"));
        if (!tenant)
            return failLine(tenant.error());
        question.books = Tenancy::booksOf(library, *tenant);
    } else if (const auto book = bookNamed(library, words.first())) {
        question.books = {book->id};
    } else {
        return failLine(book.error());
    }
    const auto hits = MeaningSearch::search(library, *route, question);
    if (!hits)
        return failLine(hits.error().text());
    QJsonArray found;
    for (const MeaningIndex::Hit &hit : *hits)
        found.append(MeaningSearch::toJson(hit));
    if (parser.isSet("json"))
        printJson(QJsonDocument(found));
    else
        for (const MeaningIndex::Hit &hit : *hits)
            printLine(stdout, u"%1\t%2\t%3%\t%4\t%5"_s.arg(hit.bookId)
                                  .arg(hit.from.section)
                                  .arg(qRound(hit.from.fraction * 100))
                                  .arg(MeaningSearch::toJson(hit).value("by"_L1).toString(), hit.text.left(200)));
    return hits->isEmpty() ? 1 : 0;
}

int forget(const Library &library, const QString &word) {
    const auto book = bookNamed(library, word);
    if (!book)
        return failLine(book.error());
    const auto forgot = MeaningIndex::want(library, book->id, false);
    return forgot ? 0 : failLine(forgot.error().text());
}

// `index <book>...` the books named; `update` those due.
int indexAsked(const Library &library, const QStringList &words, bool json) {
    const Settings settings;
    const auto route = AiRegistry::embeddings(settings);
    if (!route)
        return failLine(route.error().text());
    QList<qint64> books;
    for (const QString &word : words.mid(1)) {
        const auto book = bookNamed(library, word);
        if (!book)
            return failLine(book.error());
        books << book->id;
    }
    return indexBooks(library, *route, words.first() == "update"_L1 ? MeaningSearch::due(library, settings, route->model) : books, json);
}

} // namespace

int runFindCli(const QStringList &arguments) {
    QCommandLineParser parser;
    common(parser, u"Find words in a book's text."_s);
    parser.addPositionalArgument("book", "A book's number in the library, or a book file.");
    parser.addPositionalArgument("words", "The words to find, as they appear.", "<words...>");
    if (!parsed(parser, arguments, u"find"_s, 2))
        return 1;
    // A book file is found without a library.
    const auto library = openLibrary(parser);
    const QString book = parser.positionalArguments().first();
    QString error;
    const auto document = BookOps::open(library ? library->get() : nullptr, book, &error);
    // A number asks the library, so one that can't be opened says why (#260).
    if (!document)
        return failLine(!library && Numbers::whole(book, std::numeric_limits<qint64>::min(), std::numeric_limits<qint64>::max())
                            ? library.error()
                            : error);
    const QJsonArray matches = BookOps::find(*document, parser.positionalArguments().mid(1).join(' '));
    if (parser.isSet("json"))
        printJson(QJsonDocument(matches));
    else
        for (const auto &match : matches) {
            const QJsonObject found = match.toObject();
            printLine(stdout, u"%1\t%2"_s.arg(found.value("section"_L1).toInt()).arg(found.value("snippet"_L1).toString()));
        }
    return matches.isEmpty() ? 1 : 0;
}

int runMeaningCli(const QStringList &arguments) {
    QCommandLineParser parser;
    common(parser, u"Find a book's text by what it means, not only its words: \"where do they cross the river?\". A book is indexed "
                   "first, by the embedding model of your own server (Ollama's nomic-embed-text unless the settings say another), and "
                   "kept up to date as its file changes; a hosted server is sent a book's text only when --hosted on allows it. A "
                   "search goes no further than where you have read to, unless --whole.\n\n"
                   "  meaning                         Where the embeddings are made, and the books indexed\n"
                   "  meaning index <book>...         Index these books, and keep them up to date\n"
                   "  meaning update                  Index again the books asked for (or every book, with --all on) that changed\n"
                   "  meaning search <book|all> <words...>  Find by meaning and words, in a book or a collection's books\n"
                   "  meaning forget <book>           Forget a book's index\n"
                   "  meaning use [--url u] [--model m] [--hosted on|off] [--all on|off]  Change where and what is indexed"_s);
    parser.addPositionalArgument("action", "index, update, search, forget or use; nothing for the state of the index.", "[action ...]");
    parser.addOption({"whole", "Search the whole book, past where you have read to."});
    parser.addOption({"collection", "With all: the books of this top-level collection (its tenant).", "name"});
    parser.addOption({"limit", "How many to list at most (10).", "n"});
    parser.addOption({"url", "The server that makes the embeddings (OpenAI-compatible, ending in /v1); empty for the AI server.", "url"});
    parser.addOption({"model", "The embedding model; empty for nomic-embed-text.", "model"});
    parser.addOption({"hosted", "Whether a hosted server may be sent a book's text: on or off.", "on|off"});
    parser.addOption({"all", "Whether every book is indexed, not only those asked for: on or off.", "on|off"});
    if (!parsed(parser, arguments, u"meaning"_s, 0))
        return 1;
    const QStringList words = parser.positionalArguments();
    const QString action = words.value(0);
    if (action == "use"_L1)
        return use(parser);
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const Library &opened = **library;
    if (action.isEmpty())
        return status(opened, Settings(), parser.isSet("json"));
    if (action == "search"_L1 && words.size() >= 3)
        return search(opened, parser, words.mid(1));
    if (action == "forget"_L1 && words.size() == 2)
        return forget(opened, words.at(1));
    if ((action == "index"_L1 && words.size() >= 2) || (action == "update"_L1 && words.size() == 1))
        return indexAsked(opened, words, parser.isSet("json"));
    printLine(stderr, parser.helpText().trimmed());
    return 1;
}
