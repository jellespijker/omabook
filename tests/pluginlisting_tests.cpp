// The plugins in Omabook's own list (share/omabook/plugins.json): each is
// offered by its name with nothing asked of GitHub, and installs by that name.
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "fakegithub.h"
#include "pluginfinder.h"
#include "pluginsources.h"
#include "plugintools.h"
#include "scripttools.h"
#include "settings.h"

class PluginListingTests : public QObject {
    Q_OBJECT

    // A home of the tests' own: nothing here reads or writes the user's settings or plugins.
    QTemporaryDir home;

private slots:
    void initTestCase() {
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", home.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", home.filePath("cache").toUtf8());
    }

    void notebooksIsOfferedAndAddedByItsName() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        FakeGitHub github;
        const PluginTools::Context context{dir.filePath("plugins"), &settings, {}};

        // Offered from the list Omabook ships, with nothing asked of GitHub.
        PluginFinder finder(context);
        finder.setDiscovery(false);
        const QJsonObject found = PluginSources::wait([&](PluginSources::Done done) { finder.search("notebook", done); }, 10000);
        QCOMPARE(found.value("results").toArray().size(), 1);
        const QJsonObject offered = found.value("results").toArray().first().toObject();
        QCOMPARE(offered.value("name").toString(), QString("notebooks"));
        QCOMPARE(offered.value("address").toString(), QString("jellespijker/omabook-notebooks"));
        QVERIFY(offered.value("listed").toBool() && offered.value("reviewed").toBool() && offered.value("installable").toBool());
        QCOMPARE(offered.value("permissions").toArray(), (QJsonArray{"files", "microphone", "clipboard"}));
        const QJsonObject format = finder.listed("notebooks").value("formats").toArray().first().toObject();
        QCOMPARE(format.value("extension").toString(), QString("omanotebook"));
        QVERIFY(github.asked.isEmpty());

        // And installed by that name, from its repository.
        Made notebooks;
        notebooks.title = "Notebooks";
        notebooks.permissions = QStringList{"files", "microphone", "clipboard"};
        notebooks.script = "omabook.format.register({name: \"Notebook\", extensions: [\"omanotebook\"], open: function () {}});\n"
                           "omabook.on(\"book.opened\", function (event) {});\n";
        github.push("jellespijker/omabook-notebooks", "main", sha('a'), notebooks.packed("jellespijker-omabook-notebooks-aaaaaaa"));
        PluginSources sources(context);
        PluginSources::Service service;
        service.api = github.api();
        service.hosts = QStringList{"127.0.0.1"};
        service.httpsOnly = false;
        sources.setService(service);
        PluginSources::AddOptions options;
        options.enable = true;
        const QJsonObject added = PluginSources::wait([&](PluginSources::Done done) { sources.add("notebooks", options, done); }, 10000);
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QCOMPARE(added.value("repository").toString(), QString("jellespijker/omabook-notebooks"));
        QVERIFY(QFile::exists(dir.filePath("plugins/notebooks/main.js")));
    }
};

QTEST_MAIN(PluginListingTests)
#include "pluginlisting_tests.moc"
