// omabook — an ebook library and reader for Omarchy.

#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QInputDevice>
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <tuple>
#include <utility>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include "aiprivacy.h"
#include "airegistry.h"
#include "assistant.h"
#include "backend.h"
#include "booklink.h"
#include "cli.h"
#include "converter.h"
#include "customization.h"
#include "desktopnotice.h"
#include "devicedesk.h"
#include "folio.h"
#include "hubdesk.h"
#include "hublinks.h"
#include "hubstorebridge.h"
#include "importer.h"
#include "interface.h"
#include "librarian.h"
#include "librarymigrations.h"
#include "libraryupgrade.h"
#include "linkinbox.h"
#include "log.h"
#include "manual.h"
#include "momentsnapshot.h"
#include "pluginformats.h"
#include "portalfilepicker.h"
#include "reader.h"
#include "readingpassages.h"
#include "secretstore.h"
#include "serveservice.h"
#include "settings.h"
#include "snapshotdesk.h"
#include "snapshotdestinations.h"
#include "syncwait.h"
#include "theme.h"
#include "themechoice.h"
#include "translator.h"

namespace {

// A command draws no window: it needs no display, whatever the desktop says.
void drawNoWindow() {
    // What the desktop had set, for a command that opens a window of its
    // own (Folio's "open"): that window must not inherit "offscreen".
    if (!qEnvironmentVariableIsSet("OMABOOK_DESKTOP_QPA"))
        qputenv("OMABOOK_DESKTOP_QPA", qgetenv("QT_QPA_PLATFORM").isEmpty() ? QByteArray("-") : qgetenv("QT_QPA_PLATFORM"));
    qputenv("QT_QPA_PLATFORM", "offscreen");
}

// A library named here, or found by the book being opened, is this window's;
// which library is in use from now on is changed only on purpose: in the
// window's Libraries, or with omabook libraries --use. A book file that is in
// a library (an entry of its view in the file manager, or a file in the
// library's own folders) is that book, found by where it is: it opens in its
// own library where it was left, without the file being read through to
// recognise it.
std::optional<Folio::Found> chooseLibrary(Settings &settings, const QCommandLineParser &arguments, const QString &book) {
    // Today's choice of AI becomes each capability's, once (ADR 0008): an
    // older config.json keeps what it chose, and only one written new starts local.
    AiPrivacy::carryOver(settings);
    if (arguments.isSet("library"))
        settings.useLibraryForSession(arguments.value("library"));
    const auto known = book.isEmpty() || arguments.isSet("library") ? std::nullopt : Folio::find(settings, book);
    if (known && QDir::cleanPath(known->library) != QDir::cleanPath(QFileInfo(settings.libraryPath()).absoluteFilePath()))
        settings.useLibraryForSession(known->library);
    return known;
}

// A library in an older format is brought up to date before the window opens
// it, on a worker, while a small window says so (#54): on a large library the
// migration and the log's baseline take a while. Before there is any other
// window or anything else on this thread, so nothing waits on the loop here
// but this.
void bringUpToDate(const QString &root, Look &theme) {
    LibraryUpgrade upgrade;
    if (!upgrade.start(root))
        return;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("upgrade", &upgrade);
    QQmlComponent message(&engine, QUrl("qrc:/Upgrading.qml"));
    const std::unique_ptr<QObject> window(message.create());
    QEventLoop waiting;
    QObject::connect(&upgrade, &LibraryUpgrade::finished, &waiting, &QEventLoop::quit);
    if (upgrade.running())
        waiting.exec();
}

// The reader follows the Calibre plugin's converter and the graph's setting,
// and finds the open book's highlights again when its file changed (#80).
void setUpReader(Reader &reader, const Backend &backend, const CalibrePlugin &calibre, Settings &settings) {
    reader.setConverter(calibre.converter(), calibre.switchedOff());
    reader.marks()->setGraphMode([&settings] { return settings.graph(); });
    // The reader's, gone with it; the page shows its highlights where they were found.
    const auto *passages = new ReadingPassages(&reader, &reader);
    QObject::connect(passages, &ReadingPassages::anchored, reader.highlights(), &ReadingHighlights::refresh);
    QObject::connect(&backend, &Backend::pluginsChanged, &reader,
                     [&reader, &calibre] { reader.setConverter(calibre.converter(), calibre.switchedOff()); });
}

// Automatic sync with the hubs (#191), as the window does it: its state beside the links, the
// desktop asked whether to wait and told of a refusal, and the background service the person's own.
HubDesk::Automatic automaticSync() {
    return {.folder = HubLinks::usualFolder(),
            .now = [] { return QDateTime::currentDateTimeUtc(); },
            .wait = SyncWait::now,
            .notify = [](const QString &title,
                         const QString &body) { std::ignore = DesktopNotice::show(title, body); }, // Shown in the panel too.
            .service = ServeService::usual(),
            .every = std::chrono::seconds(30),
            .times = {}};
}

// The hubs' syncs bring books in; the library's changes, the person at work in the window
// and the network coming back are what automatic sync waits for (#191).
void followHubs(HubDesk &hubDesk, const Backend &backend, QGuiApplication &app) {
    QObject::connect(&hubDesk, &HubDesk::booksChanged, &backend, &Backend::reload);
    QObject::connect(&backend, &Backend::revisionChanged, &hubDesk, &HubDesk::libraryChanged);
    app.installEventFilter(&hubDesk);
    SyncWait::whenOnline(&hubDesk, [&hubDesk] { hubDesk.cameIntoReach(); });
}

// A command says what went wrong in English, which scripts can rely on;
// --translated says it in the desktop's language, as the window does.
int runCommand(QStringList words) {
    // As the window does at its start (chooseLibrary).
    Settings first;
    AiPrivacy::carryOver(first);
    std::optional<Translator> translator;
    if (words.removeAll("--translated") > 0) {
        translator.emplace();
        QCoreApplication::installTranslator(&*translator);
    }
    if (const QString problem = first.problem(); !problem.isEmpty())
        std::println(stderr, "omabook: {}", problem.toStdString());
    const int unsaved = Settings::unsaved();
    const int result = runCli(words);
    if (Settings::unsaved() == unsaved)
        return result;
    std::println(stderr, "omabook: {}", Settings::tr("The change was not saved.").toStdString());
    return result == 0 ? 1 : result;
}

// A link to a place in a book (#138) opens it there, and brings the window forward.
void openLink(const QString &link, Backend &backend, Reader &reader, QWindow *window) {
    const auto target = BookLink::parse(link);
    if (!target)
        return;
    const qint64 id = backend.library() ? BookLink::find(*backend.library(), target->book) : 0;
    if (id <= 0)
        backend.say(QObject::tr("The book that link opens isn't in this library."));
    else if (target->section >= 0)
        reader.openBookAt(id, target->section, 0);
    else
        reader.openBook(id);
    if (window) {
        window->show();
        window->raise();
        window->requestActivate();
    }
}

// Links clicked elsewhere come to this window, unless another listens already;
// `first` is the one it was opened with, if any.
void followLinks(LinkInbox &links, const QString &first, Backend &backend, Reader &reader, const Interface &interface) {
    links.listen();
    QObject::connect(&links, &LinkInbox::linked, &reader,
                     [&backend, &reader, &interface](const QString &given) { openLink(given, backend, reader, interface.window()); });
    if (!first.isEmpty())
        openLink(first, backend, reader, interface.window());
}

// The desktop entry hands over a file as a file:// address, and a link to a
// book as itself: the file is left as its path, a link taken out.
QString takeLink(QString &book) {
    if (QUrl(book).isLocalFile())
        book = QUrl(book).toLocalFile();
    return BookLink::parse(book) ? std::exchange(book, QString()) : QString();
}

// What isn't a command is the window's, whose one word is a book: the word
// that is none (no file, no book's number, no link to one), if there is one.
std::optional<QString> unknownWord(const QStringList &given) {
    static const QStringList valued{"--library", "--screenshot", "--look-up", "--model"};
    qsizetype at = given.value(1) == "open" ? 2 : 1;
    while (at < given.size() && given.at(at).startsWith('-'))
        at += valued.contains(given.at(at)) ? 2 : 1;
    QString word = given.value(at);
    bool number = false;
    word.toLongLong(&number);
    const bool link = BookLink::parse(word) || QUrl(word).isLocalFile();
    if (word.isEmpty() || number || link || QFileInfo::exists(word))
        return std::nullopt;
    return word;
}

// What is said, before anything is set up, of arguments that can't be meant;
// nothing when they can. `--library "$UNSET"` is a mistake, not the folder
// omabook runs in (#234). What isn't a command is the window's, whose one word
// is a book: a word that is neither is said to be that, before any window,
// rather than leaving the window to complain about an option a command would take.
std::optional<QString> refusal(const QStringList &given, bool command) {
    const auto unnamed = [](const QString &option, const QString &value) { return option == "--library" && value.isEmpty(); };
    if (given.contains("--library=") || std::ranges::adjacent_find(given, unnamed) != given.end())
        return "--library needs a folder.";
    if (const auto word = command ? std::nullopt : unknownWord(given))
        return "\"" + *word + "\" is not a command, a book file, a book's number or a link to one. Run omabook for the commands.";
    return std::nullopt;
}

// What the window reads, by the names it reads them by.
void expose(const QQmlEngine &engine, std::initializer_list<std::pair<const char *, QObject *>> objects) {
    for (const auto &[name, object] : objects)
        engine.rootContext()->setContextProperty(name, object);
}

// What the window and every command set up first.
void startUp(bool command) {
    // What happens is written to the log (omabook logs), from the window and
    // from every command alike.
    Log::install(command ? "cli" : "window");
    // A book in the format of a plugin that isn't installed here is kept, and
    // says which plugin reads it.
    PluginFormats::know();
    // A library brought up to date takes a moment snapshot first, where snapshots are set up (#208).
    LibraryMigrations::beforeUpgrading(MomentSnapshot::beforeUpgradeHere);
    // A destination of the snapshots on another of the person's hubs is reached over the session with it (#196).
    SnapshotDestinations::reachHubsBy(HubStoreBridge::linked());
    if (!command)
        qCInfo(lcApp).noquote() << "Omabook" << OMABOOK_VERSION << "started";
}

// The book named on the command line, to open, and what bringing it in said.
struct ToOpen {
    qint64 id = 0;
    ImportSummary added;
};

// One the library has, by its id or found by its file's content; else it
// joins the library first.
ToOpen bookToOpen(const QString &book, const std::optional<Folio::Found> &known, Backend &backend) {
    bool isNumber = false;
    const qint64 id = known ? known->id : book.toLongLong(&isNumber);
    if (book.isEmpty() || isNumber || known || !backend.library())
        return {.id = id, .added = {}};
    // Through the importer, so a folder that is one book (Markdown
    // chapters, say) opens like a file does.
    ImportSummary added = importBooks(*backend.library(), {QFileInfo(book).absoluteFilePath()});
    backend.reload();
    return {.id = added.lastId, .added = std::move(added)};
}

// The book is opened, and then said what bringing it in said.
void openGiven(const ToOpen &given, Reader &reader, Backend &backend) {
    if (given.id > 0)
        reader.openBook(given.id);
    backend.reportOpened(given.added);
}

// Whether what was asked draws no window: a command, or the help or the version.
bool drawsNoWindow(const QStringList &given, bool command) {
    return command || std::ranges::any_of(given.mid(1), [](const QString &argument) {
               return argument == "--help" || argument == "-h" || argument == "--version" || argument == "-v";
           });
}

// The arguments as the program was given them, before there is an application to ask.
QStringList asGiven(std::span<char *const> arguments) {
    QStringList given;
    for (const char *argument : arguments)
        given << QString::fromLocal8Bit(argument);
    return given;
}

} // namespace

int main(int argc, char *argv[]) {
    // Omabook themes itself from Omarchy's palette; the desktop's Qt platform
    // theme would only fight it.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");

    // Commands and help draw no window, so they must not need a display, even
    // where the desktop exports QT_QPA_PLATFORM=wayland. Bare `omabook` prints
    // help, as a command line tool should; the launcher says `omabook open`.
    // Options every command takes may come before the command's name too:
    // `omabook --library ~/Other list` is `omabook list --library ~/Other`.
    const QStringList given = commandFirst(asGiven({argv, static_cast<std::size_t>(argc)}));
    const bool command = argc == 1 || isCliCommand(given.value(1));
    const bool windowless = drawsNoWindow(given, command);
    if (const auto why = refusal(given, command)) {
        std::println(stderr, "omabook: {}", why->toStdString());
        return 1;
    }
    if (windowless)
        drawNoWindow();

    QGuiApplication app(argc, argv);
    app.setApplicationName("omabook");
    app.setApplicationVersion(OMABOOK_VERSION);
    startUp(command);
    if (command)
        return runCommand(commandFirst(QCoreApplication::arguments()));

    QCommandLineParser arguments;
    arguments.setApplicationDescription("An ebook library and reader.\n\n" + cliSummary());
    arguments.addHelpOption();
    arguments.addVersionOption();
    arguments.addPositionalArgument("book", "A book file, or a book's number in the library, to open.", "[book]");
    arguments.addOption({"library", "Open the library in this folder in this window; the one in use stays as it is "
                                    "(omabook libraries --use changes that).", "folder"});
    QCommandLineOption screenshot("screenshot", "Save a picture of the window and quit.", "file");
    screenshot.setFlags(QCommandLineOption::HiddenFromHelp);
    arguments.addOption(screenshot);
    // Folio's "Look Up Details" on a book's entry: the window opens with the
    // look-up for that book.
    QCommandLineOption lookUp("look-up", "Look up the details of the book with this number.", "id");
    lookUp.setFlags(QCommandLineOption::HiddenFromHelp);
    arguments.addOption(lookUp);
    QStringList words = commandFirst(app.arguments());
    if (words.value(1) == "open")
        words.removeAt(1);
    if (!arguments.parse(words)) {
        fprintf(stderr, "omabook: %s\nRun omabook --help for what the window takes, or omabook for the commands.\n",
                qPrintable(arguments.errorText()));
        return 1;
    }
    if (arguments.isSet("help"))
        arguments.showHelp();
    if (arguments.isSet("version"))
        arguments.showVersion();
    QString book = arguments.positionalArguments().value(0);
    const QString link = takeLink(book);
    // A window already open takes a link, and then this one is done.
    if (!link.isEmpty() && LinkInbox::handOver(link))
        return 0;

    // Matches the window to omabook.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The compiled-in copy covers
    // a run from build/, and needs qt6-svg's icon engine.
    app.setDesktopFileName("omabook");
    app.setWindowIcon(QIcon::fromTheme("omabook", QIcon(":/omabook.svg")));
    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme desktopTheme;
    // Everything in ~/.config/omabook is followed while the app runs, and
    // the guide there tells whoever edits it (a person, an agent) how.
    Customization custom;
    custom.writeGuide();
    // Which theme the colors come from: style.json's "theme", Omarchy's by default.
    ThemeChoice themeChoice(&custom, ThemePlaces::here(custom.folder()));
    Look theme(&desktopTheme, &themeChoice, &custom);
    Settings settings;
    PortalFilePicker picker;
    const auto known = chooseLibrary(settings, arguments, book);
    bringUpToDate(settings.libraryPath(), theme);
    Backend backend(&settings, &picker);
    backend.fileManager()->setActive(true);
    // The help, the tour and what is new, which remember what was shown.
    Manual manual(&settings, &custom);
    // Converting is the Calibre plugin's, so there is no converter while it is off.
    const CalibrePlugin &calibre = backend.pluginRegistry()->calibre();
    Reader reader(&backend, calibre.converter());
    setUpReader(reader, backend, calibre, settings);
    Assistant assistant(&reader, AiRegistry::answerer(settings));
    QObject::connect(&settings, &Settings::changed, &assistant, &Assistant::refresh);
    // Whether the knowledge graph grows while reading is the user's choice.
    assistant.setGraphMode([&settings] { return settings.graph(); });
    // Automatic means Omarchy's default agent, followed while the app runs:
    // `omarchy default agent codex` changes who the companion is at once.
    AiAgent aiAgent(&settings);
    QObject::connect(&aiAgent, &AiAgent::changed, &assistant, &Assistant::refresh);
    Librarian librarian(&backend, &settings, &custom, [&theme] { return theme.colors(); });
    // E-readers and phones (#71): the server they talk to, KOReader's sync and sending them books.
    DeviceDesk deviceDesk(&settings, Secrets::shared(), [&backend] { return backend.libraryPath(); });
    QObject::connect(&backend, &Backend::libraryChanged, &deviceDesk, &DeviceDesk::follow);
    QObject::connect(&deviceDesk, &DeviceDesk::booksChanged, &backend, &Backend::reload);
    deviceDesk.follow();
    // The library's snapshots (#185): taken, compared, gone back to, and where else they are kept.
    SnapshotDesk snapshotDesk(SnapshotPlace::here(), SnapshotContents::here(), [&backend] { return backend.libraryPath(); });
    QObject::connect(&snapshotDesk, &SnapshotDesk::booksChanged, &backend, &Backend::reload);
    // The hubs this computer is linked to (#189), through their sessions, synced with by themselves where switched on (#191).
    HubDesk hubDesk(HubDesk::linkedHere([&backend] { return backend.libraryPath(); }), automaticSync());
    followHubs(hubDesk, backend, app);
    // What script plugins add to the keyboard and the window joins the
    // interface's own, and follows the plugins as they are edited.
    const auto followPlugins = [&custom, &backend] { custom.setPlugins(backend.scripts()->commands(), backend.scripts()->panels()); };
    QObject::connect(backend.scripts(), &ScriptPlugins::changed, &custom, followPlugins);
    followPlugins();

    const ToOpen opening = bookToOpen(book, known, backend);

    QQmlEngine engine;
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QCoreApplication::quit);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.addImageProvider("book", reader.imageProvider());
    expose(engine, {{"translator", &translator}, {"theme", &theme},         {"settings", &settings}, {"backend", &backend},
                    {"reader", &reader},         {"assistant", &assistant}, {"aiAgent", &aiAgent},   {"custom", &custom},
                    {"librarian", &librarian},   {"manual", &manual},       {"devices", &deviceDesk},
                    {"snapshotDesk", &snapshotDesk}, {"hubsDesk", &hubDesk}});
    // Where there is a touchscreen, what the keys do is also on screen.
    const auto devices = QInputDevice::devices();
    const bool touchscreen = std::ranges::contains(devices, QInputDevice::DeviceType::TouchScreen, &QInputDevice::type);
    engine.rootContext()->setContextProperty("touchscreen", touchscreen);
    // Pages are still being drawn on worker threads when the window closes;
    // let them finish while the application they draw with still exists.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] { QThreadPool::globalInstance()->waitForDone(); });
    Interface interface(&engine, &custom);
    if (!interface.load())
        return 1;
    openGiven(opening, reader, backend);
    LinkInbox links;
    followLinks(links, link, backend, reader, interface);
    if (const qint64 id = arguments.value(lookUp).toLongLong(); id > 0 && interface.window())
        QMetaObject::invokeMethod(interface.window(), "lookUpBook", Q_ARG(QVariant, id));
    // For the README's picture and for looking at a change without a desktop.
    if (arguments.isSet(screenshot)) {
        QTimer::singleShot(1500, &app, [&app, &interface, &arguments, &screenshot] {
            app.exit(interface.window() && interface.window()->grabWindow().save(arguments.value(screenshot)) ? 0 : 1);
        });
    }

    return app.exec();
}
