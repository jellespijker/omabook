// A library that is expected and isn't there (a drive unplugged, a share not
// mounted, a folder moved away) is said to be missing, and none is made in its
// place without asking (#221): the commands, the MCP server, a library held
// open, and the window.

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QtTest>

#include "desk.h"
#include "library.h"
#include "mcpharness.h"

namespace {

[[nodiscard]] bool madeAnything(const QString &folder) {
    return QFileInfo::exists(folder) && !QDir(folder).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
}

[[nodiscard]] bool click(QQuickWindow &window, QQuickItem *item) {
    if (!item || !Fixtures::scrolledIntoSight(window, *item))
        return false;
    const QPointF middle = item->mapToScene({item->width() / 2, item->height() / 2});
    QTest::mouseClick(&window, Qt::LeftButton, {}, middle.toPoint());
    return true;
}

} // namespace

class LibraryMissingTests : public QObject {
    Q_OBJECT

private slots:
    void init() { QFile::remove(Settings::defaultFile()); }

    void commandsSayAConfiguredLibraryIsNotThere() {
        const McpHarness::Machine machine;
        const QString drive = machine.dir.filePath("usb"), away = machine.dir.filePath("usb.unplugged");
        const QString library = drive + "/Library";
        QCOMPARE(machine.omabook({"libraries", "--use", library, "--create"}).code, 0);
        const QString book = machine.dir.filePath("book.fb2");
        QVERIFY(Fixtures::write(book, Fixtures::fb2()));
        QCOMPARE(machine.omabook({"add", book}).code, 0);
        QVERIFY(QDir().rename(drive, away));

        for (const QStringList &arguments : {QStringList{"list"}, QStringList{"add", book}, QStringList{"list", "--library", library}}) {
            const auto ran = machine.omabook(arguments);
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains(library) && ran.errors.contains("--create"), qPrintable(ran.errors));
            QVERIFY(!QFileInfo::exists(drive));
        }
        QVERIFY(machine.omabook({"libraries"}).output.contains("(not there)"));
        // The empty folder a drive is mounted on is no library either.
        QVERIFY(QDir().mkpath(library));
        QCOMPARE(machine.omabook({"list"}).code, 1);
        QVERIFY(!madeAnything(library));
        // Asking for one there makes one.
        QCOMPARE(machine.omabook({"libraries", "--use", library, "--create"}).code, 0);
        QCOMPARE(machine.omabook({"list"}).code, 0);
        QVERIFY(QDir(drive).removeRecursively());
        QVERIFY(QDir().rename(away, drive));
        QVERIFY(machine.omabook({"list"}).output.contains("Fiction Fixture"));
    }

    void theFirstStartMakesTheUsualLibraryOnce() {
        const McpHarness::Machine machine;
        const QString books = machine.dir.filePath("home/Books");
        QCOMPARE(machine.omabook({"list"}).code, 0);
        QVERIFY(QFileInfo::exists(books + "/omabook.db"));
        QVERIFY(QDir().rename(books, books + ".away"));
        QCOMPARE(machine.omabook({"list"}).code, 1);
        QVERIFY(!QFileInfo::exists(books));
    }

    void aLibraryNamedInTheSettingsIsNotMade() {
        const McpHarness::Machine machine;
        const QString library = machine.dir.filePath("share/Library");
        QVERIFY(Fixtures::write(machine.dir.filePath("home/.config/omabook/config.json"),
                                QByteArray("{\"ai_provider\": \"off\", \"library\": \"") + library.toUtf8() + "\"}"));
        QCOMPARE(machine.omabook({"list"}).code, 1);
        QVERIFY(!QFileInfo::exists(machine.dir.filePath("share")));
    }

    // As before: --library with a folder nobody knows of yet starts one there.
    void aNewFolderNamedForOneRunIsMade() {
        const McpHarness::Machine machine;
        QCOMPARE(machine.omabook({"list", "--library", machine.library}).code, 0);
        QVERIFY(QFileInfo::exists(machine.library + "/omabook.db"));
    }

    void theMcpServerSaysItsLibraryWentAway() {
        const McpHarness::Machine machine;
        McpHarness::Server server(machine);
        server.request("initialize", {{"protocolVersion", "2025-06-18"}});
        QCOMPARE(server.failure("books_search"), QString());
        QVERIFY(QDir().rename(machine.library, machine.library + ".away"));
        const QString why = server.failure("books_search");
        QVERIFY2(why.contains(machine.library), qPrintable(why));
        QVERIFY(!server.failure("books_search").isEmpty());
        QVERIFY(!QFileInfo::exists(machine.library));
        QVERIFY(QDir().rename(machine.library + ".away", machine.library));
        QCOMPARE(server.failure("books_search"), QString());
    }

    void aLibraryHeldOpenWritesNothingWhereItWas() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library"), book = dir.filePath("book.fb2");
        QVERIFY(Fixtures::write(book, Fixtures::fb2()));
        Library library(root);
        QVERIFY(library.open());
        const qint64 id = library.addEntry({.title = "On paper"}, true).id;
        QVERIFY(id > 0);
        QVERIFY(QDir().rename(root, root + ".away"));
        QVERIFY(!library.addEntry({.title = "Another"}, true).error.isEmpty());
        QVERIFY(!library.add(book).error.isEmpty());
        QImage cover(10, 20, QImage::Format_RGB32);
        cover.fill(Qt::red);
        QVERIFY(!library.setCover(id, cover));
        QVERIFY(!library.addFormat(id, book));
        QVERIFY(!QFileInfo::exists(root));
        Library again(root);
        QVERIFY(!again.open());
        QVERIFY(!QFileInfo::exists(root));
    }

    void theWindowSaysSoWhenItsLibraryGoesAway() {
        Desk desk("en", {}, false);
        const QString root = desk.settings->libraryPath(), away = root + ".away";
        QVERIFY(desk.backend->newEntry({{"title", "On paper"}, {"paper", true}}) > 0);
        QVERIFY(desk.show());
        QVERIFY(QDir().rename(root, away));
        QTRY_VERIFY(desk.backend->property("libraryMissing").toBool());
        QTRY_VERIFY(desk.shows("libraryMissing"));
        QCOMPARE(desk.backend->newEntry({{"title", "Another"}, {"paper", true}}), 0);
        desk.backend->reload();
        QVERIFY(!QFileInfo::exists(root));
        // Trying again finds it still gone, and then back.
        QVERIFY(click(*desk.window, desk.item("libraryRetry")));
        QVERIFY(desk.backend->property("libraryMissing").toBool());
        QVERIFY(!QFileInfo::exists(root));
        QVERIFY(QDir().rename(away, root));
        QVERIFY(click(*desk.window, desk.item("libraryRetry")));
        QTRY_VERIFY(!desk.backend->property("libraryMissing").toBool());
        QCOMPARE(desk.backend->property("libraryError").toString(), QString());
        QTRY_VERIFY(!desk.shows("libraryMissing"));
        QCOMPARE(desk.find("library")->property("visible").toBool(), true);
        // Or a new one is made there, because it was asked for.
        QVERIFY(QDir().rename(root, away));
        QTRY_VERIFY(desk.shows("libraryMissing"));
        QVERIFY(click(*desk.window, desk.item("libraryCreate")));
        QTRY_VERIFY(!desk.backend->property("libraryMissing").toBool());
        QVERIFY(QFileInfo::exists(root + "/omabook.db"));
        QCOMPARE(desk.warnings, QStringList());
    }

    // The settings that name the person's library can't be read (#222): the
    // usual folder is not made in its place.
    void anUnreadableConfigMakesNoLibrary() {
        const McpHarness::Machine machine;
        const QString books = machine.dir.filePath("home/Books"), library = machine.dir.filePath("share/Library");
        const QString config = machine.dir.filePath("home/.config/omabook/config.json");
        QVERIFY(Fixtures::write(config, QByteArray("{\"ai_provider\": \"off\", \"library\": \"") + library.toUtf8() + "\",}"));
        const QString book = machine.dir.filePath("book.fb2");
        QVERIFY(Fixtures::write(book, Fixtures::fb2()));

        for (const QStringList &arguments : {QStringList{"list"}, QStringList{"add", book}, QStringList{"libraries"}}) {
            const auto ran = machine.omabook(arguments);
            QVERIFY2(ran.errors.contains(config), qPrintable(ran.errors));
            QVERIFY(!QFileInfo::exists(books));
        }
        QCOMPARE(machine.omabook({"list"}).code, 1);
        McpHarness::Server server(machine);
        server.request("initialize", {{"protocolVersion", "2025-06-18"}});
        QVERIFY(!server.failure("books_search").isEmpty());
        QVERIFY(!QFileInfo::exists(machine.library));
        QVERIFY(!QFileInfo::exists(books));
    }

    // A library that is there opens as before, and nothing is said unsaved.
    void anUnreadableConfigLeavesTheUsualLibraryUsable() {
        const McpHarness::Machine machine;
        QCOMPARE(machine.omabook({"list"}).code, 0);
        QVERIFY(Fixtures::write(machine.dir.filePath("home/.config/omabook/config.json"), "{\"ai_provider\": \"off\",}"));
        const auto ran = machine.omabook({"list"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
    }

    void theWindowMakesNoLibraryWhileTheConfigCantBeRead() {
        QTemporaryDir home;
        const QByteArray usual = qgetenv("HOME"), settingsAt = qgetenv("XDG_CONFIG_HOME");
        qputenv("HOME", home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home.filePath(".config").toUtf8());
        const auto restored = qScopeGuard([&usual, &settingsAt] {
            qputenv("HOME", usual);
            qputenv("XDG_CONFIG_HOME", settingsAt);
        });
        QVERIFY(QDir().mkpath(QFileInfo(Settings::defaultFile()).absolutePath()));
        QVERIFY(Fixtures::write(Settings::defaultFile(), "{\"ai_provider\": \"off\",}"));
        Settings settings;
        QCOMPARE(settings.libraryPath(), home.filePath("Books"));
        NoPicker picker;
        Backend backend(
            &settings, &picker, nullptr, [](QString *) -> std::unique_ptr<AiProvider> { return nullptr; },
            [](QObject *) { return QList<MetadataSource *>(); }, home.filePath("plugins"), "-");
        QVERIFY(backend.property("libraryMissing").toBool());
        QVERIFY(!QFileInfo::exists(home.filePath("Books")));
        // Only asking for one there makes one.
        QVERIFY(QMetaObject::invokeMethod(&backend, "createLibrary"));
        QVERIFY(QFileInfo::exists(home.filePath("Books/omabook.db")));
    }

    void theWindowMakesALibraryOnlyWhereOneIsAskedFor() {
        QTemporaryDir dir;
        const QString gone = dir.filePath("gone/Library"), fresh = dir.filePath("fresh");
        QVERIFY(QDir().mkpath(QFileInfo(Settings::defaultFile()).absolutePath()));
        QVERIFY(Fixtures::write(Settings::defaultFile(), QByteArray("{\"ai_provider\": \"off\", \"library\": \"") + gone.toUtf8() + "\"}"));
        Settings settings;
        NoPicker picker;
        Backend backend(
            &settings, &picker, nullptr, [](QString *) -> std::unique_ptr<AiProvider> { return nullptr; },
            [](QObject *) { return QList<MetadataSource *>(); }, dir.filePath("plugins"), "-");
        QVERIFY(backend.property("libraryMissing").toBool());
        QVERIFY(!QFileInfo::exists(dir.filePath("gone")));
        // A folder chosen that no library was ever in becomes one.
        backend.useLibrary(fresh);
        QVERIFY(!backend.property("libraryMissing").toBool());
        QVERIFY(QFileInfo::exists(fresh + "/omabook.db"));
        // One of the list that isn't there doesn't.
        backend.useLibrary(gone);
        QVERIFY(backend.property("libraryMissing").toBool());
        QVERIFY(!QFileInfo::exists(dir.filePath("gone")));
        QVERIFY(QMetaObject::invokeMethod(&backend, "createLibrary"));
        QVERIFY(!backend.property("libraryMissing").toBool());
        QVERIFY(QFileInfo::exists(gone + "/omabook.db"));
    }
};

QTEST_MAIN(LibraryMissingTests)
#include "librarymissing_tests.moc"
