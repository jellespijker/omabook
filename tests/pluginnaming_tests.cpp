#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "fakegithub.h"
#include "fixtures.h"
#include "pluginsources.h"
#include "plugintools.h"
#include "scripttools.h"
#include "settings.h"

// The name a plugin is installed under: the same whether it came from a
// folder, an archive or its repository on GitHub, and one installed under
// the name a folder used to keep is replaced, not doubled (#20).

namespace {

const char *const manifest = "{\"name\": \"Storyteller\", \"version\": \"1.0\", \"script\": \"main.js\", \"permissions\": [\"files\"]}";
const char *const script = "omabook.on(\"book.closed\", function (event) {});\n";

// A plugin's folder as a checkout of its repository is: named after it.
bool checkout(const QString &folder) {
    return QDir().mkpath(folder) && Fixtures::write(folder + "/plugin.json", manifest) && Fixtures::write(folder + "/main.js", script);
}

// Everything in the plugin folder, hidden or not.
QStringList leftIn(const QString &folder) {
    return QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

// A plugin folder and settings of the test's own.
struct Place {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    PluginTools::Context context{dir.filePath("plugins"), &settings, {}};

    QString nameOf(const QString &source) const {
        const QJsonObject installed = PluginTools::install(context, source);
        return ScriptTools::failed(installed) ? installed.value("error").toString() : installed.value("name").toString();
    }
};

} // namespace

class PluginNamingTests : public QObject {
    Q_OBJECT

    // A home of the tests' own: what is replaced goes to its trash, not the user's.
    QTemporaryDir home;

private slots:
    void initTestCase() {
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", home.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", home.filePath("cache").toUtf8());
    }

    void aPluginHasOneNameHoweverItCame() {
        QTemporaryDir sources;
        QVERIFY(checkout(sources.filePath("omabook-storyteller")));
        QVERIFY(Fixtures::zip(sources.filePath("omabook-storyteller.zip"), {{"plugin.json", manifest}, {"main.js", script}}));
        QVERIFY(Fixtures::zip(sources.filePath("download.zip"),
                              {{"omabook-storyteller/plugin.json", manifest}, {"omabook-storyteller/main.js", script}}));
        QVERIFY(checkout(sources.filePath("omabook-plugin-reading-log")));

        QCOMPARE(Place().nameOf(sources.filePath("omabook-storyteller")), QString("storyteller"));
        QCOMPARE(Place().nameOf(sources.filePath("omabook-storyteller.zip")), QString("storyteller"));
        QCOMPARE(Place().nameOf(sources.filePath("download.zip")), QString("storyteller"));
        QCOMPARE(Place().nameOf(sources.filePath("omabook-plugin-reading-log")), QString("reading-log"));

        // Its repository on GitHub gives it the same name.
        Place place;
        FakeGitHub github;
        github.push("someone/omabook-storyteller", "main", sha('a'), Made().packed());
        PluginSources hub(place.context);
        PluginSources::Service service;
        service.api = github.api();
        service.hosts = QStringList{"127.0.0.1"};
        service.httpsOnly = false;
        hub.setService(service);
        const QJsonObject added =
            PluginSources::wait([&](PluginSources::Done done) { hub.add("someone/omabook-storyteller", {}, done); }, 10000);
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QCOMPARE(added.value("name").toString(), QString("storyteller"));

        // A name that is given is the name.
        Place named;
        PluginTools::InstallOptions options;
        options.name = "omabook-storyteller";
        QCOMPARE(PluginTools::install(named.context, sources.filePath("omabook-storyteller"), options).value("name").toString(),
                 QString("omabook-storyteller"));
    }

    void oneInstalledUnderItsOldNameIsReplacedNotDoubled() {
        QTemporaryDir sources;
        QVERIFY(checkout(sources.filePath("omabook-storyteller")));
        const QString from = sources.filePath("omabook-storyteller");
        Place place;
        // As an install from the folder used to name it, with what it kept of its own.
        PluginTools::InstallOptions old;
        old.name = "omabook-storyteller";
        old.enable = true;
        QVERIFY(!ScriptTools::failed(PluginTools::install(place.context, from, old)));
        QVERIFY(QDir().mkpath(place.dir.filePath("plugins/omabook-storyteller/data")));
        QVERIFY(Fixtures::write(place.dir.filePath("plugins/omabook-storyteller/data/story.json"), "[1]"));

        // Looked at, it is the one installed.
        const QJsonObject seen = PluginTools::inspect(place.context, from);
        QCOMPARE(seen.value("name").toString(), QString("storyteller"));
        QVERIFY(seen.value("installed").toBool());
        QCOMPARE(seen.value("formerly").toString(), QString("omabook-storyteller"));

        // It is in the way unless it is to be replaced, as one of the same name is.
        const QString refused = PluginTools::install(place.context, from).value("error").toString();
        QVERIFY2(refused.contains("already") && refused.contains("omabook-storyteller"), qPrintable(refused));
        QCOMPARE(leftIn(place.dir.filePath("plugins")), QStringList{"omabook-storyteller"});

        // Replaced, there is one, under the new name, with what the old one kept.
        PluginTools::InstallOptions again;
        again.replace = again.enable = true;
        const QJsonObject replaced = PluginTools::install(place.context, from, again);
        QVERIFY2(!ScriptTools::failed(replaced), qPrintable(replaced.value("error").toString()));
        QVERIFY(replaced.value("replaced").toBool());
        QCOMPARE(replaced.value("id").toString(), QString("folder:storyteller"));
        QCOMPARE(leftIn(place.dir.filePath("plugins")), QStringList{"storyteller"});
        QVERIFY(QFile::exists(place.dir.filePath("plugins/storyteller/data/story.json")));
        QVERIFY(place.settings.pluginEnabled("folder:storyteller", false));
        QVERIFY(ScriptTools::check(place.context, "storyteller").value("ok").toBool());

        // From then on it is replaced under its own name.
        QVERIFY(PluginTools::inspect(place.context, from).value("formerly").toString().isEmpty());
        QVERIFY(!ScriptTools::failed(PluginTools::install(place.context, from, again)));
        QCOMPARE(leftIn(place.dir.filePath("plugins")), QStringList{"storyteller"});
    }
};

QTEST_MAIN(PluginNamingTests)
#include "pluginnaming_tests.moc"
