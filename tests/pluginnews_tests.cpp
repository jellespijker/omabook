// A plugin told when its data came from elsewhere (#126): after a sync with
// another computer, a plugin hears `log.changed` of the things of its own that
// came, once for the sync, never of another plugin's or of its own writes; an
// operation stored however it came (a space, one kept until known) is told the
// same; and a plugin's open view is handed it and shows it.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "desk.h"
#include "fixtures.h"
#include "library.h"
#include "plugindata.h"
#include "remote.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "spacereplay.h"
#include "sync.h"

using namespace Qt::StringLiterals;

namespace {

using Fixtures::whyNot;

// A plugin with no view, that counts what it is told of its data.
constexpr const char *counter = R"(
var told = [];
omabook.on("log.changed", function (event) { told.push(event.entities); });
function heard() { return told; }
function count(id) { omabook.log.set("tally", id, {n: 1}); return true; }
)";

void writePlugin(const QString &folder, const QString &name, const QByteArray &script, const QByteArray &view = {}) {
    QVERIFY(QDir().mkpath(folder + "/" + name));
    const QJsonObject manifest{{"name", name}, {"version", "1.0"}, {"script", "main.js"}};
    QVERIFY(Fixtures::write(folder + "/" + name + "/plugin.json", QJsonDocument(manifest).toJson()));
    QVERIFY(Fixtures::write(folder + "/" + name + "/main.js", script));
    if (!view.isEmpty())
        QVERIFY(Fixtures::write(folder + "/" + name + "/View.qml", view));
}

SyncSummary syncWith(Library &here, const QTemporaryDir &dir, const QString &there) {
    const QByteArray standIn = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8();
    const QString ssh = Fixtures::script(dir.filePath("ssh-current"), "exec '" + standIn + "' \"$@\"\n");
    return syncLibrary(here, Remote("laptop:" + there, {ssh}));
}

[[nodiscard]] PluginData::Thing thing(const QString &plugin, const QString &entity, const QString &id) {
    return {.plugin = plugin, .entity = entity, .id = id};
}

// One computer with two counters running on its library, and another computer's library beside it.
struct TwoComputers {
    TwoComputers() : folder(dir.filePath("plugins")), here(dir.filePath("Here")), there(dir.filePath("There")) {
        QVERIFY(here.open() && there.open());
        writePlugin(folder, "counter", counter);
        writePlugin(folder, "scores", counter);
        ScriptServices services;
        services.library = [this] { return &here; };
        plugins = std::make_unique<ScriptPlugins>(services, folder, nullptr, false);
        QVERIFY(plugins->plugin("counter") && plugins->plugin("scores"));
        // The window's first look: where the log is now.
        plugins->lookAtTheLog();
    }

    QJsonArray heard(const QString &plugin) {
        QString problem;
        const QJsonValue answer = plugins->plugin(plugin)->callNow("heard", {}, &problem);
        [&] { QVERIFY2(problem.isEmpty(), qPrintable(problem)); }();
        return answer.toArray();
    }

    SyncSummary sync() { return syncWith(here, dir, dir.filePath("There")); }

    QTemporaryDir dir;
    QString folder;
    Library here, there;
    std::unique_ptr<ScriptPlugins> plugins;
};

// A plugin with a view that shows what it was last handed.
const QByteArray viewScript = R"js(
omabook.ui.view({id: "pages", title: "Pages", source: "View.qml", key: "Ctrl+Shift+P"});
function show() { omabook.ui.showView("pages", {}); }
)js";

const QByteArray view = R"(import QtQuick
import QtQuick.Controls

Item {
    id: pages
    objectName: "pagesView"
    property string news: ""
    function logChanged(entities) { news = (entities.page || []).join(",") }
    Label { objectName: "pagesNews"; text: "News: " + pages.news }
}
)";

} // namespace

class PluginNewsTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // What another computer wrote of each plugin's comes at a sync, and each
    // plugin hears once of its own things alone; what it wrote itself, here,
    // is no news to it, and a sync that brings nothing tells nothing.
    void aSyncTellsEachPluginOfItsOwn() {
        TwoComputers two;
        QString problem;
        QVERIFY(two.plugins->plugin("counter")->callNow("count", {"mine"}, &problem).toBool());
        two.plugins->lookAtTheLog();
        QTest::qWait(500);
        QCOMPARE(two.heard("counter"), QJsonArray());

        QVERIFY(whyNot(PluginData::set(two.there, thing("counter", "tally", "b"), {{"n", 2}})).isEmpty());
        QVERIFY(whyNot(PluginData::set(two.there, thing("counter", "tally", "a"), {{"n", 3}})).isEmpty());
        QVERIFY(whyNot(PluginData::change(two.there, thing("counter", "round", "r1"), "goals", {.added = {{"g1", 1}}, .removed = {}}))
                    .isEmpty());
        QVERIFY(whyNot(PluginData::set(two.there, thing("scores", "tally", "x"), {{"n", 4}})).isEmpty());
        // Of a plugin that isn't here: kept, and told to nobody.
        QVERIFY(whyNot(PluginData::set(two.there, thing("absent", "tally", "z"), {{"n", 5}})).isEmpty());
        const SyncSummary summary = two.sync();
        QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(), qPrintable(summary.error + summary.problems.join('\n')));
        // The window looks more than once while a sync writes; it is told as one.
        two.plugins->lookAtTheLog();
        two.plugins->lookAtTheLog();

        QTRY_COMPARE(two.heard("counter").size(), 1);
        QCOMPARE(two.heard("counter").first().toObject(), (QJsonObject{{"round", QJsonArray{"r1"}}, {"tally", QJsonArray{"a", "b"}}}));
        QTRY_COMPARE(two.heard("scores").size(), 1);
        QCOMPARE(two.heard("scores").first().toObject(), (QJsonObject{{"tally", QJsonArray{"x"}}}));
        // The data is there to read when it is told.
        QCOMPARE(PluginData::list(two.here, "counter", "tally").size(), 3);

        const SyncSummary again = two.sync();
        QVERIFY2(again.error.isEmpty(), qPrintable(again.error));
        two.plugins->lookAtTheLog();
        QTest::qWait(500);
        QCOMPARE(two.heard("counter").size(), 1);
        QCOMPARE(two.heard("scores").size(), 1);
    }

    // However an operation of a plugin's came into the log (a space, or one
    // kept until this Omabook knew its kind, are stored as a sync's are), the
    // plugin hears of it; a library opened in place of this one is no news.
    void storedFromElsewhereIsTold() {
        TwoComputers two;
        const SpaceOperation op{.space = PluginData::spaceOf(u"counter"),
                                .device = u"d-elsewhere"_s,
                                .seq = 1,
                                .hlc = 1000,
                                .author = u"d-elsewhere"_s,
                                .entity = u"counter:tally"_s,
                                .id = u"t1"_s,
                                .field = u"n"_s,
                                .value = u"7"_s};
        SpaceReplay::store(two.here, QList<SpaceOperation>{op});
        two.plugins->lookAtTheLog();
        QTRY_COMPARE(two.heard("counter").size(), 1);
        QCOMPARE(two.heard("counter").first().toObject(), (QJsonObject{{"tally", QJsonArray{"t1"}}}));

        // Another library, with data of its own from elsewhere: only marked.
        Library other(two.dir.filePath("Other"));
        QVERIFY(other.open());
        SpaceOperation more = op;
        more.seq = 2;
        SpaceReplay::store(other, QList<SpaceOperation>{more});
        ScriptServices services;
        services.library = [&other] { return &other; };
        ScriptPlugins switched(services, two.folder, nullptr, false);
        switched.lookAtTheLog();
        switched.lookAtTheLog();
        QTest::qWait(500);
        QString problem;
        QCOMPARE(switched.plugin("counter")->callNow("heard", {}, &problem).toArray(), QJsonArray());
    }

    // In the window: a sync from the command line (another connection to the
    // library) brings pages, and the plugin's open view is handed what came
    // and shows it, without the plugin doing anything.
    void anOpenViewShowsWhatCame() {
        Desk desk("en", {}, false);
        writePlugin(desk.dir.filePath("plugins"), "pages", viewScript, view);
        desk.backend->scripts()->reload();
        QVERIFY(QTest::qWaitFor([&] { return desk.custom->keyText().contains("library.pages:pages"); }, 3000));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QString problem;
        desk.backend->scripts()->plugin("pages")->callNow("show", {}, &problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QTRY_VERIFY(desk.shows("pagesView"));
        QCOMPARE(desk.find("pagesNews")->property("text").toString(), QString("News: "));

        {
            Library there(desk.dir.filePath("There"));
            QVERIFY(there.open());
            QVERIFY(whyNot(PluginData::set(there, thing("pages", "page", "p2"), {{"title", "Tuesday"}})).isEmpty());
            QVERIFY(whyNot(PluginData::set(there, thing("pages", "page", "p1"), {{"title", "Monday"}})).isEmpty());
        }
        Library mine(desk.settings->libraryPath());
        QVERIFY(mine.open());
        const SyncSummary summary = syncWith(mine, desk.dir, desk.dir.filePath("There"));
        QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(), qPrintable(summary.error + summary.problems.join('\n')));
        QTRY_COMPARE_WITH_TIMEOUT(desk.find("pagesNews")->property("text").toString(), QString("News: p1,p2"), 5000);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(PluginNewsTests)
#include "pluginnews_tests.moc"
