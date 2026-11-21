// What a plugin needs for recordings beside its ink (#135), as the notebooks
// plugin's recordings do: a test microphone and a yes to recording given
// beforehand for `omabook plugin run` in a scratch library only, strokes
// hidden or standing out by id on the ink surface, files that travel to the
// other computer only when they are opened there, and a notice in the window.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <tuple>

#include "audiodevices.h"
#include "desk.h"
#include "fixtures.h"
#include "inknodes.h"
#include "inksurface.h"
#include "library.h"
#include "opusreader.h"
#include "pluginfiles.h"
#include "pluginfilesync.h"
#include "remote.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "scripttools.h"
#include "settings.h"
#include "sync.h"
#include "testaudio.h"

namespace {

// Records a moment, plays it back to the end, and answers what was kept.
const QByteArray recorder = R"js(
function record(file) {
    return omabook.audio.record({file: file}).then(function (made) {
        return new Promise(function (resolve) {
            omabook.after(600, function () {
                var summary = made.stop();
                omabook.audio.play(summary.file, {onEnded: function () { resolve({duration: summary.duration, ended: true}); }});
            });
        });
    });
}
function attempt(file) {
    return omabook.audio.record({file: file}).then(function (made) { made.stop(); return "recorded"; },
                                                    function (error) { return String(error.message); });
}
function mark(path, on) { return omabook.files.onDemand(path, on); }
function status(path) { return omabook.files.status(path); }
function fetch(path) {
    return omabook.files.fetch(path).then(function (here) { return {here: here, text: omabook.files.read(here)}; },
                                          function (error) { return {error: String(error.message)}; });
}
var pressed = 0;
function tell(words) { omabook.ui.notify(words, {action: "Play", run: function () { pressed++; }}); }
function presses() { return pressed; }
function badNotice() { omabook.ui.notify("Kept", {run: function () {}}); }
)js";

bool writePlugin(const QString &plugins, const QString &id, const QStringList &permissions) {
    const QString folder = plugins + "/" + id;
    const QJsonObject manifest{{"name", "Notes"}, {"script", "main.js"}, {"permissions", QJsonArray::fromStringList(permissions)}};
    return QDir().mkpath(folder + "/data") && Fixtures::write(folder + "/plugin.json", QJsonDocument(manifest).toJson())
           && Fixtures::write(folder + "/main.js", recorder);
}

// What a function of the script answered, a promise waited for.
QJsonValue answer(ScriptPlugin &plugin, const QString &function, const QJsonArray &arguments = {}) {
    bool done = false;
    QJsonValue result;
    plugin.callFunction(function, arguments, [&](const QJsonValue &value, const QString &problem) {
        result = problem.isEmpty() ? value : QJsonValue("problem: " + problem);
        done = true;
    });
    for (int i = 0; i < 1000 && !done; ++i)
        QTest::qWait(10);
    return result;
}

QString text(const Desk &desk, const QString &name) {
    const QObject *found = desk.find(name);
    return found ? found->property("text").toString() : QString();
}

Remote otherComputer(const QTemporaryDir &dir, const QString &library) {
    return Remote("laptop:" + library, {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
}

QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class PluginRecordingTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The test microphone hears a tone as a real one would, and what plays
    // ends as it would: a recording made and played back with no sound card,
    // the yes given beforehand. Without it nobody can be asked.
    void aTestMicrophoneRecordsWithTheYesGivenBeforehand() {
        QTemporaryDir dir;
        QVERIFY(writePlugin(dir.filePath("plugins"), "notes", {"microphone", "files"}));
        const auto hears = TestAudio::microphone("tone");
        QVERIFY(hears.has_value());
        AudioDevices room(*hears, [] { return TestAudio::speakers(); });
        ScriptServices services;
        services.audio = &room;
        services.microphoneGranted = true;
        ScriptPlugin plugin(ScriptManifest::read(dir.filePath("plugins/notes")), services);
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        const QJsonObject made = answer(plugin, "record", {"data/moment.opus"}).toObject();
        QVERIFY2(made.value("ended").toBool(), qPrintable(QJsonDocument(made).toJson()));
        QVERIFY(made.value("duration").toDouble() >= 400);
        auto reader = Opus::Reader::open(dir.filePath("plugins/notes/data/moment.opus"));
        QVERIFY(reader.has_value());
        QVERIFY((*reader)->duration().count() >= 400);

        ScriptServices asking = services;
        asking.microphoneGranted = false;
        ScriptPlugin unasked(ScriptManifest::read(dir.filePath("plugins/notes")), asking);
        QVERIFY2(unasked.load(&problem), qPrintable(problem));
        QVERIFY(answer(unasked, "attempt", {"data/other.opus"}).toString().contains("Nobody is here to say yes"));

        // What it hears is a tone, silence or a WAV file of a recording's kind; nothing else.
        QVERIFY(TestAudio::microphone("silence").has_value());
        QVERIFY(!TestAudio::microphone(dir.filePath("none.wav")).has_value());
        QVERIFY(Fixtures::write(dir.filePath("not.wav"), "not a sound"));
        QVERIFY(!TestAudio::microphone(dir.filePath("not.wav")).has_value());
    }

    // `omabook plugin run` records with them in a scratch library, and
    // refuses them on the person's own.
    void pluginRunRecordsOnlyInAScratchLibrary() {
        QTemporaryDir dir;
        const QString plugins = dir.filePath("plugins");
        QVERIFY(writePlugin(plugins, "notes", {"microphone", "files"}));
        Settings own;
        own.setLibraryPath(dir.filePath("Own"));
        const QStringList recording{"--test-microphone", "tone", "--allow-microphone", "--folder", plugins};
        const auto run = [](const QStringList &arguments) {
            return runPluginCli(QStringList{"omabook", "plugin", "run", "notes", "record"} + arguments);
        };
        QCOMPARE(run(QStringList{R"(["data/a.opus"])", "--library", dir.filePath("Scratch")} + recording), 0);
        auto reader = Opus::Reader::open(plugins + "/notes/data/a.opus");
        QVERIFY(reader.has_value());
        QVERIFY((*reader)->duration().count() >= 400);

        QCOMPARE(run(QStringList{R"(["data/b.opus"])", "--library", dir.filePath("Own")} + recording), 1);
        QCOMPARE(run(QStringList{R"(["data/b.opus"])"} + recording), 1);
        QVERIFY(!QFileInfo::exists(plugins + "/notes/data/b.opus"));
        QCOMPARE(run({R"(["data/b.opus"])", "--library", dir.filePath("Scratch"), "--test-microphone", dir.filePath("nothing.wav"),
                      "--folder", plugins}),
                 1);
    }

    // Strokes hidden are not drawn and those that stand out are drawn in the
    // colour of emphasis, their own alpha kept; the strokes stay as they are.
    void strokesAreHiddenOrStandOutById() {
        std::vector<Ink::Stroke> strokes(3);
        strokes[0].id = "a";
        strokes[1].id = "b";
        strokes[1].color = QColor(0, 0, 0, 128);
        strokes[2].id = "c";
        std::vector<const Ink::Stroke *> shown{&strokes[0], &strokes[1], &strokes[2]};
        const QSet<QString> hidden{"a"}, emphasized{"b", "gone"};
        std::vector<Ink::Stroke> styled;
        InkNodes::styled(shown, {.hidden = hidden, .emphasized = emphasized, .emphasis = QColor(Qt::red)}, styled);
        QCOMPARE(shown.size(), std::size_t{2});
        QCOMPARE(shown[0]->id, QString("b"));
        QCOMPARE(shown[0]->color.red(), 255);
        QCOMPARE(shown[0]->color.alpha(), 128);
        QVERIFY(shown[1] == &strokes[2]);
        QCOMPARE(strokes[1].color, QColor(0, 0, 0, 128));

        InkSurface surface;
        QSignalSpy styledSpy(&surface, &InkSurface::styleChanged);
        surface.setHiddenStrokes({"a", "a"});
        surface.setHiddenStrokes({"a"});
        surface.setEmphasizedStrokes({"b"});
        surface.setEmphasisColor(Qt::blue);
        surface.setEmphasisColor(QColor());
        QCOMPARE(styledSpy.size(), 3);
        QCOMPARE(surface.hiddenStrokes(), QStringList{"a"});
        QCOMPARE(surface.emphasizedStrokes(), QStringList{"b"});
        QCOMPARE(surface.emphasisColor(), QColor(Qt::blue));
    }

    // A file marked on demand isn't fetched at a sync, nor sent: each side
    // remembers where it is, and it comes when the plugin asks for it.
    void aFileOnDemandComesWhenItIsOpened() {
        QTemporaryDir dir;
        const QString mine = dir.filePath("plugins");
        const QString theirs = dir.filePath("there-home/.config/omabook/plugins");
        QVERIFY(writePlugin(mine, "notes", {"files"}));
        QVERIFY(writePlugin(theirs, "notes", {"files"}));
        QVERIFY(QDir().mkpath(theirs + "/notes/data/recordings") && QDir().mkpath(mine + "/notes/data/recordings"));
        QVERIFY(Fixtures::write(theirs + "/notes/data/recordings/monday.opus", "a long recording"));
        QVERIFY(Fixtures::write(mine + "/notes/data/recordings/tuesday.opus", "another long one"));
        QVERIFY(Fixtures::write(mine + "/notes/data/page.txt", "a page"));
        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        {
            Library there(dir.filePath("There"));
            QVERIFY(there.open());
            QVERIFY(PluginFiles::setOnDemand(there, "notes", "recordings/", true));
        }
        const Remote remote = otherComputer(dir, dir.filePath("There"));
        SyncOptions options;
        options.plugins = mine;
        SyncSummary summary = syncLibrary(here, remote, {}, options);
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY(PluginFiles::isOnDemand(PluginFiles::onDemand(here, "notes"), "recordings/monday.opus"));
        QVERIFY(!PluginFiles::isOnDemand(PluginFiles::onDemand(here, "notes"), "page.txt"));
        QCOMPARE(summary.pluginFilesSent, 1); // the page, not tuesday's recording
        QCOMPARE(summary.pluginFilesTaken, 0);
        QCOMPARE(summary.pluginFilesOnDemand, 1);
        QVERIFY(!QFileInfo::exists(mine + "/notes/data/recordings/monday.opus"));
        QVERIFY(!QFileInfo::exists(theirs + "/notes/data/recordings/tuesday.opus"));
        QVERIFY(PluginFiles::elsewhere(mine, "notes").contains("recordings/monday.opus"));
        QVERIFY(PluginFiles::elsewhere(theirs, "notes").contains("recordings/tuesday.opus"));

        // The plugin asks, and it comes; again, it is here.
        ScriptServices services;
        services.library = [&here] { return &here; };
        services.fetchFile = [&](const QString &plugin, const QString &path, const std::function<void(bool)> &done) {
            const auto fetched = PluginFileSync::fetch(here, mine, plugin, path, {}, {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
            done(fetched == PluginFileSync::Fetched::Here || fetched == PluginFileSync::Fetched::Fetched);
        };
        ScriptPlugin plugin(ScriptManifest::read(mine + "/notes"), services);
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        QCOMPARE(answer(plugin, "status", {"data/recordings/monday.opus"}).toObject(),
                 (QJsonObject{{"here", false}, {"elsewhere", true}, {"onDemand", true}}));
        const QJsonObject fetched = answer(plugin, "fetch", {"data/recordings/monday.opus"}).toObject();
        QCOMPARE(fetched.value("text").toString(), QString("a long recording"));
        QCOMPARE(contents(mine + "/notes/data/recordings/monday.opus"), QByteArray("a long recording"));
        QVERIFY(!PluginFiles::elsewhere(mine, "notes").contains("recordings/monday.opus"));
        QCOMPARE(answer(plugin, "status", {"data/recordings/monday.opus"}).toObject().value("here").toBool(), true);
        QVERIFY(answer(plugin, "fetch", {"data/nothing.opus"}).toObject().value("error").toString().contains("couldn't be fetched"));

        // Marked by the plugin, then not: it travels at every sync again.
        QVERIFY(answer(plugin, "mark", {"data/recordings", false}).toBool());
        QVERIFY(!PluginFiles::isOnDemand(PluginFiles::onDemand(here, "notes"), "recordings/tuesday.opus"));
        summary = syncLibrary(here, remote, {}, options);
        QCOMPARE(contents(theirs + "/notes/data/recordings/tuesday.opus"), QByteArray("another long one"));
    }

    // A notice is said at the bottom of the window with its button, which
    // calls the plugin once; with no window it is said as a plugin says things.
    void aNoticeSaysSomethingWithAButton() {
        Desk desk("en", {}, false);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(writePlugin(desk.dir.filePath("plugins"), "notes", {}));
        desk.backend->scripts()->reload();
        QTRY_VERIFY(desk.backend->scripts()->plugin("notes") != nullptr);
        ScriptPlugin &plugin = *desk.backend->scripts()->plugin("notes");
        std::ignore = answer(plugin, "tell", {"Recording kept"});
        QTRY_COMPARE(text(desk, "toastText"), QString("Recording kept"));
        QVERIFY(desk.shows("toastAction"));
        QCOMPARE(text(desk, "toastAction"), QString("Play"));
        desk.backend->actOnMessage();
        QTRY_COMPARE(answer(plugin, "presses").toInt(), 1);
        QVERIFY(desk.backend->message().isEmpty());
        desk.backend->actOnMessage();
        QCOMPARE(answer(plugin, "presses").toInt(), 1);
        // Another message has no button.
        desk.backend->say("Bookmarked");
        QVERIFY(desk.backend->messageAction().isEmpty());
        QVERIFY(answer(plugin, "badNotice").toString().contains("says on its button"));

        QTemporaryDir dir;
        QVERIFY(writePlugin(dir.filePath("plugins"), "notes", {}));
        ScriptPlugin alone(ScriptManifest::read(dir.filePath("plugins/notes")), {});
        QSignalSpy said(&alone, &ScriptPlugin::said);
        QString problem;
        QVERIFY2(alone.load(&problem), qPrintable(problem));
        std::ignore = answer(alone, "tell", {"Recording kept"});
        QCOMPARE(said.size(), 1);
        QCOMPARE(said.first().first().toString(), QString("Recording kept"));
    }
};

QTEST_MAIN(PluginRecordingTests)
#include "pluginrecording_tests.moc"
