#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include "fixtures.h"
#include "mcpharness.h"

// A plugin that can't load is neither installed nor switched on, and is said
// not to be (#289): by the command line and to MCP clients, without running
// any of its script. Everything is in a temporary folder.

namespace {

// A plugin of the machine's own to install from, and where it lands.
struct World {
    McpHarness::Machine machine;
    QString source = machine.dir.filePath("zz");
    QString installed = machine.dir.filePath("home/.config/omabook/plugins/zz");

    [[nodiscard]] bool write(const QString &folder, const QByteArray &script, const QByteArray &name = "zz") const {
        return QDir().mkpath(folder)
               && Fixtures::write(folder + "/plugin.json", "{\"name\": \"" + name + "\", \"script\": \"main.js\", \"permissions\": []}")
               && Fixtures::write(folder + "/main.js", script);
    }
};

const QByteArray broken = "var fine = 1;\nthis is ( not js\n";

} // namespace

class PluginLoadTests : public QObject {
    Q_OBJECT

private slots:
    void aScriptThatDoesntParseIsNotInstalled() {
        const World world;
        QVERIFY(world.write(world.source, broken));
        for (const QStringList &arguments :
             {QStringList{"plugin", "install", world.source, "--enable"}, QStringList{"plugin", "install", world.source}}) {
            const auto ran = world.machine.omabook(arguments);
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains("main.js:2: SyntaxError"), qPrintable(ran.output + ran.errors));
            QVERIFY2(!ran.output.contains("is on"), qPrintable(ran.output));
            QVERIFY(!QDir(world.installed).exists());
        }
    }

    // Written straight into the plugin folder, as one being made is.
    void aScriptThatDoesntParseIsNotSwitchedOn() {
        const World world;
        QVERIFY(world.write(world.installed, broken));
        for (const QStringList &arguments : {QStringList{"plugin", "enable", "zz"}, QStringList{"plugins", "enable", "folder:zz"}}) {
            const auto ran = world.machine.omabook(arguments);
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains("can't load") && ran.errors.contains("main.js:2: SyntaxError"),
                     qPrintable(ran.output + ran.errors));
        }
        McpHarness::Server server(world.machine);
        const QString said = server.failure("plugin_set_enabled", {{"id", "folder:zz"}, {"enabled", true}});
        QVERIFY2(said.contains("main.js:2: SyntaxError"), qPrintable(said));
        // Switching it off is always done.
        QCOMPARE(world.machine.omabook({"plugin", "disable", "zz"}).code, 0);
        QVERIFY(server.failure("plugin_set_enabled", {{"id", "folder:zz"}, {"enabled", false}}).isEmpty());
    }

    // Looking whether a script parses runs none of it.
    void aScriptThatParsesIsInstalledAndNothingOfItIsRunByLooking() {
        const World world;
        QVERIFY(world.write(world.source, "#!/usr/bin/env omabook\nfunction hoisted() { return 1; }\nthrow new Error('ran');\n"));
        const auto ran = world.machine.omabook({"plugin", "install", world.source});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(world.machine.omabook({"plugin", "enable", "zz"}).code, 0);
    }

    void aNameWrittenAsAPathIsRefused() {
        const World world;
        QVERIFY(world.write(world.source, "var fine = 1;\n", "../../escape"));
        const auto ran = world.machine.omabook({"plugin", "install", world.source});
        QCOMPARE(ran.code, 1);
        QVERIFY2(ran.errors.contains("written as a path"), qPrintable(ran.output + ran.errors));
        QVERIFY(!QDir(world.installed).exists());
        // A stroke in a name makes it no path.
        QVERIFY(world.write(world.source, "var fine = 1;\n", "EPUB/PDF notes"));
        QCOMPARE(world.machine.omabook({"plugin", "install", world.source}).code, 0);
    }
};

QTEST_GUILESS_MAIN(PluginLoadTests)
#include "pluginload_tests.moc"
