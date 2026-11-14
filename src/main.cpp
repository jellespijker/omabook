// omabook — an ebook library and reader for Omarchy.

#include <QCommandLineParser>
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
#include "importer.h"
#include "interface.h"
#include "librarian.h"
#include "manual.h"
#include "portalfilepicker.h"
#include "reader.h"
#include "settings.h"
#include "theme.h"
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
    if (windowless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("omabook");
    app.setApplicationVersion(OMABOOK_VERSION);
    if (command)
        return runCli(app.arguments());

    QCommandLineParser arguments;
    arguments.setApplicationDescription("An ebook library and reader.\n\n" + cliSummary());
    arguments.addHelpOption();
    arguments.addVersionOption();
    arguments.addPositionalArgument("book", "A book file, or a book's number in the library, to open.", "[book]");
    arguments.addOption({"library", "Open the library in this folder, and use it from now on.", "folder"});
    QCommandLineOption screenshot("screenshot", "Save a picture of the window and quit.", "file");
    screenshot.setFlags(QCommandLineOption::HiddenFromHelp);
    arguments.addOption(screenshot);
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
    Look theme(&desktopTheme, &custom);
    Settings settings;
    PortalFilePicker picker;
    if (arguments.isSet("library"))
        settings.setLibraryPath(arguments.value("library"));
    Backend backend(&settings, &picker);
    // The help, the tour and what is new, which remember what was shown.
    Manual manual(&settings, &custom);
    // Converting is the Calibre plugin's, so there is no converter while it is off.
    const CalibrePlugin &calibre = backend.pluginRegistry()->calibre();
    Reader reader(&backend, calibre.converter());
    reader.setConverter(calibre.converter(), calibre.switchedOff());
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
    const QString book = arguments.positionalArguments().value(0);
    bool isNumber = false;
    qint64 opening = book.toLongLong(&isNumber);
    if (!book.isEmpty() && !isNumber && backend.library()) {
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
    // For the README's picture and for looking at a change without a desktop.
    if (arguments.isSet(screenshot)) {
        QTimer::singleShot(1500, &app, [&app, &interface, &arguments, &screenshot] {
            app.exit(interface.window() && interface.window()->grabWindow().save(arguments.value(screenshot)) ? 0 : 1);
        });
    }

    return app.exec();
}
