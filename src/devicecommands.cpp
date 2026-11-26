#include "devicecommands.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <tuple>

#include "bookops.h"
#include "deviceoptions.h"
#include "deviceserver.h"
#include "deviceserving.h"
#include "formats.h"
#include "importer.h"
#include "koreaderdocument.h"
#include "koreadersync.h"
#include "kosyncclient.h"
#include "library.h"
#include "opdscatalog.h"
#include "secretstore.h"
#include "sendbook.h"
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

// A command's parser with the options every command has; false, with the
// help printed, for a wrong number of arguments.
[[nodiscard]] bool parsed(QCommandLineParser &parser, const QStringList &arguments, const QString &description, qsizetype atLeast,
                          qsizetype atMost) {
    parser.setApplicationDescription(description);
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library", "Use the library in this folder instead of the current one, for this run only.", "folder"});
    QStringList rest = arguments;
    rest[0] = u"omabook "_s + rest.at(1);
    rest.removeAt(1);
    parser.process(rest);
    const qsizetype given = parser.positionalArguments().size();
    if (given < atLeast || given > atMost) {
        printLine(stderr, parser.helpText().trimmed());
        return false;
    }
    return true;
}

[[nodiscard]] QString libraryPath(const QCommandLineParser &parser) {
    return parser.isSet("library") ? parser.value("library") : Settings().libraryPath();
}

[[nodiscard]] std::expected<std::unique_ptr<Library>, QString> openLibrary(const QCommandLineParser &parser) {
    auto library = std::make_unique<Library>(libraryPath(parser));
    library->onBringingUpToDate([](const QString &said) { printLine(stderr, said); });
    QString error;
    if (!library->open(&error))
        return std::unexpected(error);
    return library;
}

// A password, read from standard input so it is in no shell's history.
[[nodiscard]] QString passwordFromInput() {
    printLine(stderr, u"Password (one line on standard input):"_s);
    return QTextStream(stdin).readLine();
}

void keepOptions(Settings &settings, const DeviceOptions &options) { settings.setDevices(options.toJson()); }

int koreaderLogin(const QStringList &words, Settings &settings) {
    if (words.size() != 3)
        return failLine(u"Say the server and your name: omabook koreader login https://sync.koreader.rocks reader"_s);
    const QString key = KosyncClient::keyOf(passwordFromInput());
    QNetworkAccessManager network;
    KosyncClient client(network, QUrl(words.at(1)), words.at(2), key);
    if (const auto allowed = client.authorize(); !allowed)
        return failLine(allowed.error().text());
    if (!Secrets::shared().put(QString::fromLatin1(DeviceSecrets::koreaderKey), key))
        return failLine(u"The keyring wouldn't keep the password."_s);
    DeviceOptions options = DeviceOptions::fromJson(settings.devices());
    options.syncServer = words.at(1);
    options.syncUser = words.at(2);
    keepOptions(settings, options);
    printLine(stdout, u"Logged in to %1 as %2."_s.arg(words.at(1), words.at(2)));
    return 0;
}

int koreaderServeAs(const QStringList &words, Settings &settings) {
    if (words.size() != 2)
        return failLine(u"Say the name KOReader logs in with: omabook koreader serve-as reader"_s);
    if (!Secrets::shared().put(QString::fromLatin1(DeviceSecrets::koreaderServeKey), KosyncClient::keyOf(passwordFromInput())))
        return failLine(u"The keyring wouldn't keep the password."_s);
    DeviceOptions options = DeviceOptions::fromJson(settings.devices());
    options.koreaderServe = true;
    options.koreaderServeUser = words.at(1);
    keepOptions(settings, options);
    printLine(stdout, u"KOReader logs in to Omabook as %1, when it is served (omabook serve)."_s.arg(words.at(1)));
    return 0;
}

int koreaderSync(const QStringList &words, const QCommandLineParser &parser, const Settings &settings) {
    const DeviceOptions options = DeviceOptions::fromJson(settings.devices());
    const QString key = Secrets::shared().get(QString::fromLatin1(DeviceSecrets::koreaderKey));
    if (options.syncServer.isEmpty() || options.syncUser.isEmpty() || key.isEmpty())
        return failLine(Failure(Failure::Code::SyncNotSetUp).text());
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    QList<qint64> books;
    for (const QString &word : words.mid(1))
        books << word.toLongLong();
    QNetworkAccessManager network;
    KosyncClient client(network, QUrl(options.syncServer), options.syncUser, key);
    const auto summary = KoreaderSync::run(**library, client, books);
    if (!summary)
        return failLine(summary.error().text());
    if (parser.isSet("json"))
        printJson(summary->toJson());
    else
        printLine(stdout, u"%1 books looked at: %2 moved by a device, %3 sent to %4."_s.arg(summary->books)
                              .arg(summary->taken)
                              .arg(summary->sent)
                              .arg(options.syncServer));
    return 0;
}

// What there is to say of a catalog's entry: a book with its formats, or a feed to go to.
[[nodiscard]] QString entryLine(qsizetype number, const OpdsCatalog::Entry &entry) {
    QString line = u"%1. %2"_s.arg(number).arg(entry.title);
    if (!entry.authors.isEmpty())
        line += u" by "_s + entry.authors.join(u", "_s);
    if (const auto feed = entry.feed())
        return line + u"  → "_s + feed->href.toString();
    QStringList types;
    for (const OpdsCatalog::Link &link : entry.acquisitions())
        types << link.type;
    return line + u"  ["_s + types.join(u", "_s) + ']';
}

[[nodiscard]] std::expected<QByteArray, QString> download(QNetworkAccessManager &network, const QUrl &url, QString *name = nullptr) {
    const std::unique_ptr<QNetworkReply> reply(network.get(QNetworkRequest(url)));
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError)
        return std::unexpected(reply->errorString());
    if (name)
        *name = QFileInfo(reply->url().path()).fileName();
    return reply->readAll();
}

// Downloads the book of a number in the feed's listing, and adds it to the library.
int addFromFeed(QNetworkAccessManager &network, const OpdsCatalog::Feed &feed, const QCommandLineParser &parser) {
    const qsizetype number = parser.value("add").toLongLong();
    if (number < 1 || number > feed.entries.size())
        return failLine(u"There is no book %1 in the listing."_s.arg(parser.value("add")));
    const QList<OpdsCatalog::Link> links = feed.entries.at(number - 1).acquisitions();
    // EPUB first, as the reader reads it best; else whatever there is.
    const auto epub =
        std::ranges::find_if(links, [](const OpdsCatalog::Link &link) { return link.type.startsWith(u"application/epub+zip"); });
    if (links.isEmpty())
        return failLine(u"Book %1 of the listing has nothing to download."_s.arg(number));
    QString name;
    const QUrl url = (epub != links.end() ? *epub : links.first()).href;
    const auto file = download(network, url, &name);
    if (!file)
        return failLine(Failure(Failure::Code::CatalogUnreachable, {url.toDisplayString(), file.error()}).text());
    const QTemporaryDir scratch;
    const QString path = scratch.filePath(name.isEmpty() || !Formats::isBook(name) ? u"book.epub"_s : name);
    if (QFile out(path); !out.open(QIODevice::WriteOnly) || out.write(*file) != file->size())
        return failLine(Failure(Failure::Code::NotCopied, {path, out.errorString()}).text());
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const ImportSummary added = importBooks(**library, {path});
    if (!added.problems.isEmpty())
        return failLine(added.problems.first());
    for (const Failure &note : added.unreadable)
        printLine(stderr, note.text());
    printLine(stdout, u"Added as book %1."_s.arg(added.lastId));
    return 0;
}

// Where a device finds what is served, and how it logs in.
void printServed(const DeviceServer::Setup &setup, const QString &address, const DeviceServing::Status &served) {
    if (served.opds)
        printLine(stdout,
                  u"OPDS catalog: %1/opds (any name, and the token %2 as the password), or %1/t/%2/opds"_s.arg(address, setup.opdsToken));
    if (served.koreader)
        printLine(stdout, u"KOReader sync: %1, logging in as %2"_s.arg(address, setup.options.koreaderServeUser));
}

// When an Omabook serves where this one would already: what it serves, and
// the exit code (1 when it doesn't serve all that was asked for).
[[nodiscard]] std::optional<int> servedAlready(const DeviceServer::Setup &setup) {
    const DeviceOptions &options = setup.options;
    const auto listening = DeviceReach::address(options.reach);
    if (!listening)
        return std::nullopt;
    const auto served = DeviceServing::ask(*listening, options.port);
    if (!served)
        return std::nullopt;
    const QString address = DeviceReach::shown(options.reach, *listening, options.port);
    printLine(stdout, DeviceServing::taken(*listening, options.port, address).text());
    printServed(setup, address, *served);
    if ((options.opds && !served->opds) || (options.koreaderServe && !served->koreader))
        return failLine(u"It doesn't serve all that was asked for: switch it on in the window, in Phones and e-readers, or stop "
                        "that Omabook serving first."_s);
    return 0;
}

} // namespace

int runServeCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.addOption({"reach", "Where to listen: localhost, tailnet or network; the settings' choice when not given.", "where"});
    parser.addOption({"port", "The port to listen on; the settings' (6622) when not given.", "port"});
    parser.addOption({"collection", "Serve only this collection (by its path) in the catalog.", "collection"});
    parser.addOption({"no-opds", "Don't serve the OPDS catalog."});
    parser.addOption({"koreader", "Serve KOReader's progress sync too (log in with omabook koreader serve-as first)."});
    parser.addOption({"service", "Serve as the background service the window sets up (Phones and e-readers), and say so to it."});
    if (!parsed(parser, arguments,
                u"Serve the library to e-readers and phones: an OPDS catalog, and KOReader's progress sync, until stopped."_s, 0, 0))
        return 1;
    const Settings settings;
    DeviceOptions options = DeviceOptions::fromJson(settings.devices());
    if (parser.isSet("reach")) {
        const auto reach = DeviceOptions::reachNamed(parser.value("reach"));
        if (!reach)
            return failLine(u"--reach takes localhost, tailnet or network, not \"%1\"."_s.arg(parser.value("reach")));
        options.reach = *reach;
    }
    if (options.reach == DeviceOptions::Reach::Off)
        return failLine(u"Serving is off: pass --reach localhost, tailnet or network, or choose where in the settings."_s);
    if (parser.isSet("port"))
        options.port = static_cast<quint16>(parser.value("port").toUInt());
    if (parser.isSet("collection"))
        options.collection = parser.value("collection");
    options.opds = options.opds && !parser.isSet("no-opds");
    options.koreaderServe = options.koreaderServe || parser.isSet("koreader");
    SecretStore &secrets = Secrets::shared();
    const DeviceServer::Setup setup{.options = options,
                                    .opdsToken = DeviceSecrets::token(secrets),
                                    .koreaderKey = secrets.get(QString::fromLatin1(DeviceSecrets::koreaderServeKey)),
                                    .by = parser.isSet("service") ? DeviceServing::By::Service : DeviceServing::By::Command};
    // An Omabook serving there already (the window, most often) is used, not fought over the port.
    if (!parser.isSet("service")) {
        if (const auto served = servedAlready(setup))
            return *served;
    }
    DeviceServer server(libraryPath(parser));
    const auto address = server.start(setup);
    if (!address)
        return failLine(address.error().text());
    if (address->isEmpty())
        return failLine(u"Nothing to serve: the catalog is off and so is KOReader's sync."_s);
    printServed(setup, *address, {.opds = options.opds, .koreader = options.koreaderServe});
    return QCoreApplication::exec();
}

int runKoreaderCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.addPositionalArgument("what", "login <server> <name> | serve-as <name> | sync [book ...] | hash <file>");
    if (!parsed(parser, arguments,
                u"KOReader's progress sync: log in to a sync server, sync the reading places with it, or say a file's document hash."_s, 1,
                1000))
        return 1;
    const QStringList words = parser.positionalArguments();
    Settings settings;
    if (words.first() == u"login")
        return koreaderLogin(words, settings);
    if (words.first() == u"serve-as")
        return koreaderServeAs(words, settings);
    if (words.first() == u"sync")
        return koreaderSync(words, parser, settings);
    if (words.first() == u"hash" && words.size() == 2) {
        const QString hash = KoreaderDocument::hash(words.at(1));
        if (hash.isEmpty())
            return failLine(Failure(Failure::Code::NoSuchFile, {words.at(1)}).text());
        printLine(stdout, hash);
        return 0;
    }
    return failLine(parser.helpText().trimmed());
}

int runSendCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.addPositionalArgument("id", "The book's number.");
    parser.addOption({"to", "The folder the e-reader is mounted at; the settings' when not given.", "folder"});
    parser.addOption({"kindle", "Mail it to the Kindle's address in the settings instead."});
    if (!parsed(
            parser, arguments,
            u"Send a book to an e-reader: copied onto the one mounted over USB, or mailed to a Kindle. Converted with Calibre when the device needs another format."_s,
            1, 1))
        return 1;
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const qint64 id = parser.positionalArguments().first().toLongLong();
    const DeviceOptions options = DeviceOptions::fromJson(Settings().devices());
    if (parser.isSet("kindle")) {
        const SendBook::Post post{
            .server = {.host = options.mailHost,
                       .port = options.mailPort,
                       .security = options.mailStartTls ? SmtpMail::Server::Security::StartTls : SmtpMail::Server::Security::Tls,
                       .user = options.mailUser,
                       .password = Secrets::shared().get(QString::fromLatin1(DeviceSecrets::mailPassword))},
            .from = options.mailFrom,
            .to = options.kindleAddress};
        if (const auto sent = SendBook::byMail(**library, id, post); !sent)
            return failLine(sent.error().text());
        printLine(stdout, u"Sent to %1."_s.arg(options.kindleAddress));
        return 0;
    }
    const auto copied = SendBook::toFolder(**library, id, parser.isSet("to") ? parser.value("to") : options.deviceFolder);
    if (!copied)
        return failLine(copied.error().text());
    if (parser.isSet("json"))
        printJson({{"id", id}, {"file", *copied}});
    else
        printLine(stdout, *copied);
    return 0;
}

int runConvertCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.addPositionalArgument("id", "The book's number.");
    parser.addOption({"to", "The format to make; epub when not given.", "format", "epub"});
    parser.addOption({"from", "Which of the book's formats to convert; the one the reader prefers when not given.", "format"});
    if (!parsed(parser, arguments, u"Convert a book to another format with Calibre's ebook-convert and add the result to the book; a file of that format the book had goes to the trash."_s, 1,
                1))
        return 1;
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const qint64 id = parser.positionalArguments().first().toLongLong();
    QString error;
    const QString file = BookOps::convert(**library, id, parser.value("to"), parser.value("from"), &error);
    if (file.isEmpty())
        return failLine(error);
    if (parser.isSet("json"))
        printJson({{"id", id}, {"file", file}});
    else
        printLine(stdout, file);
    return 0;
}

int runOpdsCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.addPositionalArgument("address", "The catalog's address, such as https://standardebooks.org/feeds/opds");
    parser.addOption({"add", "Add the book of this number in the listing to the library.", "number"});
    if (!parsed(parser, arguments,
                u"Browse an OPDS catalog (Standard Ebooks, Project Gutenberg, a Calibre content server), and add a book from it."_s, 1, 1))
        return 1;
    const QUrl address = QUrl::fromUserInput(parser.positionalArguments().first());
    QNetworkAccessManager network;
    const auto xml = download(network, address);
    if (!xml)
        return failLine(Failure(Failure::Code::CatalogUnreachable, {address.toDisplayString(), xml.error()}).text());
    const auto feed = OpdsCatalog::parse(*xml, address);
    if (!feed)
        return failLine(feed.error().text());
    if (!parser.isSet("add")) {
        printLine(stdout, feed->title);
        for (qsizetype i = 0; i < feed->entries.size(); ++i)
            printLine(stdout, entryLine(i + 1, feed->entries.at(i)));
        if (const auto next = feed->link(u"next"_s))
            printLine(stdout, u"More: omabook opds "_s + next->href.toString());
        return 0;
    }
    return addFromFeed(network, *feed, parser);
}
