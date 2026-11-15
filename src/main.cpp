// omabook — an ebook library and reader for Omarchy.

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QInputDevice>
#include <algorithm>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include "assistant.h"
#include "backend.h"
#include "cli.h"
#include "converter.h"
#include "customization.h"
#include "folio.h"
#include "importer.h"
#include "interface.h"
#include "log.h"
#include "librarian.h"
#include "manual.h"
#include "portalfilepicker.h"
#include "reader.h"
#include "settings.h"
#include "theme.h"
#include "themechoice.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Omabook themes itself from Omarchy's palette; the desktop's Qt platform
    // theme would only fight it.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");

    // Commands and help draw no window, so they must not need a display, even
    // where the desktop exports QT_QPA_PLATFORM=wayland. Bare `omabook` prints
    // help, as a command line tool should; the launcher says `omabook open`.
    const bool command = argc == 1 || isCliCommand(argv[1]);
    bool windowless = command;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        windowless = windowless || argument == "--help" || argument == "-h" || argument == "--version" || argument == "-v";
    }
    if (windowless) {
        // What the desktop had set, for a command that opens a window of its
        // own (Folio's "open"): that window must not inherit "offscreen".
        if (!qEnvironmentVariableIsSet("OMABOOK_DESKTOP_QPA"))
            qputenv("OMABOOK_DESKTOP_QPA", qgetenv("QT_QPA_PLATFORM").isEmpty() ? QByteArray("-") : qgetenv("QT_QPA_PLATFORM"));
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("omabook");
    app.setApplicationVersion(OMABOOK_VERSION);
    // What happens is written to the log (omabook logs), from the window and
    // from every command alike.
    Log::install(command ? "cli" : "window");
    if (!command)
        qCInfo(lcApp).noquote() << "Omabook" << OMABOOK_VERSION << "started";
    if (command)
        return runCli(app.arguments());

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
    QStringList words = app.arguments();
    if (words.value(1) == "open")
        words.removeAt(1);
    arguments.process(words);

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
    // A library named here, or found by the book being opened, is this
    // window's; which library is in use from now on is changed only on
    // purpose: in the window's Libraries, or with omabook libraries --use.
    if (arguments.isSet("library"))
        settings.useLibraryForSession(arguments.value("library"));
    // A book file that is in a library (an entry of its view in the file
    // manager, or a file in the library's own folders) is that book, found
    // by where it is: it opens in its own library where it was left, without
    // the file being read through to recognise it.
    const QString book = arguments.positionalArguments().value(0);
    const auto known = book.isEmpty() || arguments.isSet("library") ? std::nullopt : Folio::find(settings, book);
    if (known && QDir::cleanPath(known->library) != QDir::cleanPath(QFileInfo(settings.libraryPath()).absoluteFilePath()))
        settings.useLibraryForSession(known->library);
    Backend backend(&settings, &picker);
    backend.fileManager()->setActive(true);
    // The help, the tour and what is new, which remember what was shown.
    Manual manual(&settings, &custom);
    // Converting is the Calibre plugin's, so there is no converter while it is off.
    const CalibrePlugin &calibre = backend.pluginRegistry()->calibre();
    Reader reader(&backend, calibre.converter());
    reader.setConverter(calibre.converter(), calibre.switchedOff());
    reader.marks()->setGraphMode([&settings] { return settings.graph(); });
    QObject::connect(&backend, &Backend::pluginsChanged, &reader,
                     [&reader, &calibre] { reader.setConverter(calibre.converter(), calibre.switchedOff()); });
    Assistant assistant(&reader, [&settings](QString *why) { return makeAiProvider(settings, why); });
    QObject::connect(&settings, &Settings::changed, &assistant, &Assistant::refresh);
    // Whether the knowledge graph grows while reading is the user's choice.
    assistant.setGraphMode([&settings] { return settings.graph(); });
    // Automatic means Omarchy's default agent, followed while the app runs:
    // `omarchy default agent codex` changes who the companion is at once.
    AiAgent aiAgent(&settings);
    QObject::connect(&aiAgent, &AiAgent::changed, &assistant, &Assistant::refresh);
    Librarian librarian(&backend, &settings, &custom, [&theme] { return theme.colors(); });
    // What script plugins add to the keyboard and the window joins the
    // interface's own, and follows the plugins as they are edited.
    const auto followPlugins = [&custom, &backend] { custom.setPlugins(backend.scripts()->commands(), backend.scripts()->panels()); };
    QObject::connect(backend.scripts(), &ScriptPlugins::changed, &custom, followPlugins);
    followPlugins();

    // A book named on the command line joins the library (a file already in
    // it is found by its content) and opens.
    bool isNumber = false;
    qint64 opening = known ? known->id : book.toLongLong(&isNumber);
    if (!book.isEmpty() && !isNumber && !known && backend.library()) {
        // Through the importer, so a folder that is one book (Markdown
        // chapters, say) opens like a file does.
        const ImportSummary added = importBooks(*backend.library(), {QFileInfo(book).absoluteFilePath()});
        opening = added.lastId;
        if (!added.problems.isEmpty())
            backend.say(added.problems.first());
        backend.reload();
    }

    QQmlEngine engine;
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QCoreApplication::quit);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.addImageProvider("book", reader.imageProvider());
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("settings", &settings);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("reader", &reader);
    engine.rootContext()->setContextProperty("assistant", &assistant);
    engine.rootContext()->setContextProperty("aiAgent", &aiAgent);
    engine.rootContext()->setContextProperty("custom", &custom);
    engine.rootContext()->setContextProperty("librarian", &librarian);
    engine.rootContext()->setContextProperty("manual", &manual);
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
    if (opening > 0)
        reader.openBook(opening);
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
