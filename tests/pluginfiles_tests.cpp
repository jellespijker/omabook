// A plugin's files (#131): bytes under its data/ folder, held to their
// limits; files the person picks and the clipboard, only right after the
// person did something of the plugin's; and the data/ folder travelling to the
// person's other computer at a sync, once, by its content.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <optional>
#include <tuple>

#include "fixtures.h"
#include "library.h"
#include "pluginfiles.h"
#include "remote.h"
#include "scriptdesk.h"
#include "scriptplugin.h"
#include "sync.h"

namespace {

// A desktop that answers at once, as the person would: with the files and the
// clipboard a test gives it, having just acted or not.
class Desk : public ScriptDesk {
public:
    bool acted = true;
    QStringList files;
    QStringList kinds;
    QString text;
    QImage image;

    [[nodiscard]] bool justActed() const override { return acted; }
    void pickFiles(const QStringList &asked, bool, const std::function<void(const QStringList &)> &picked) override {
        kinds = asked;
        QTimer::singleShot(0, [picked, chosen = files] { picked(chosen); });
    }
    [[nodiscard]] QString clipboardText() const override { return text; }
    void setClipboardText(const QString &words) override { text = words; }
    [[nodiscard]] QImage clipboardImage() const override { return image; }
    void setClipboardImage(const QImage &picture) override { image = picture; }
};

struct Bench {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    QString folder = dir.filePath("plugins");
    Desk desk;
    bool withDesk = true;

    Bench() { library.open(); }

    QString plugin(const QString &name, const QByteArray &script, const QStringList &permissions) {
        const QString path = folder + "/" + name;
        QDir().mkpath(path);
        const QJsonObject manifest{
            {"name", name}, {"version", "1.0"}, {"script", "main.js"}, {"permissions", QJsonArray::fromStringList(permissions)}};
        Fixtures::write(path + "/plugin.json", QJsonDocument(manifest).toJson());
        Fixtures::write(path + "/main.js", script);
        return path;
    }
    ScriptServices services() {
        ScriptServices services;
        services.library = [this] { return &library; };
        services.desk = withDesk ? &desk : nullptr;
        return services;
    }
};

// What a function of the plugin answered, a promise waited for, or why it failed.
struct Answer {
    QJsonValue result;
    QString problem;
};
Answer called(ScriptPlugin &plugin, const QString &function, const QJsonArray &arguments = {}) {
    std::optional<Answer> answer;
    plugin.callFunction(function, arguments,
                        [&answer](const QJsonValue &result, const QString &problem) { answer = Answer{result, problem}; });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 5000))
        return {{}, "no answer"};
    return *answer;
}

constexpr const char *keeper = R"(
function roundTrip(png) {
    var bytes = new Uint8Array(256);
    for (var i = 0; i < 256; ++i) bytes[i] = i;
    omabook.files.writeBytes("data/all.bin", bytes);
    omabook.files.write("data/half.bin", bytes.subarray(128));
    omabook.files.writeBytes("data/pictures/dot.png", png);
    omabook.files.write("data/words.txt", "plain words");
    var back = new Uint8Array(omabook.files.readBytes("data/all.bin"));
    var same = back.length === 256 && Array.prototype.every.call(back, function (b, i) { return b === i; });
    return [same, omabook.files.readBase64("data/all.bin").length, new Uint8Array(omabook.files.readBytes("data/half.bin"))[0],
            omabook.files.read("data/words.txt"), omabook.files.readBytes("data/missing.bin") === null];
}
function outside() { omabook.files.writeBytes("../escape.bin", new Uint8Array(1)); }
function notData() { omabook.files.writeBytes("main.js", new Uint8Array(1)); }
function tooLarge() { omabook.files.writeBytes("data/big.bin", new ArrayBuffer(64 * 1024 * 1024 + 1)); }
function notBase64() { omabook.files.writeBytes("data/odd.bin", "not base64 at all!"); }
function usage() { return omabook.files.usage(); }
function removed() { return omabook.files.remove("data/all.bin"); }
)";

constexpr const char *picker = R"(
var heard = "";
omabook.on("book.added", function () {
    try { omabook.ui.pickFile(); heard = "asked"; } catch (error) { heard = "refused"; }
});
function said() { return heard; }
function pick(kinds) {
    return omabook.ui.pickFile({kinds: kinds}).then(function (file) {
        return file ? [file.name, file.size, file.type, file.width, file.copyTo("data/in/" + file.name), file.base64().length > 0] : null;
    });
}
function pickMany() {
    return omabook.ui.pickFile({multiple: true}).then(function (files) { return files.map(function (file) { return file.text(); }); });
}
function copy(words) { return omabook.ui.clipboard.writeText(words); }
function paste() { return omabook.ui.clipboard.readText(); }
function pastePicture() {
    var picture = omabook.ui.clipboard.readImage();
    return picture ? [picture.type, picture.width, picture.height, picture.copyTo("data/pasted.png") > 0] : null;
}
function copyPicture(path) { return omabook.ui.clipboard.writeImage(path ? path : omabook.files.readBytes("data/pasted.png")); }
)";

// The other computer behind a stand-in ssh, whose plugins are in there-home.
Remote otherComputer(const QTemporaryDir &dir, const QString &library) {
    return Remote("laptop:" + library, {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
}

void installed(const QString &plugins, const QString &id) {
    QVERIFY(QDir().mkpath(plugins + "/" + id + "/data"));
    QVERIFY(Fixtures::write(plugins + "/" + id + "/plugin.json", R"({"name": "Notes", "script": "main.js", "permissions": ["files"]})"));
    QVERIFY(Fixtures::write(plugins + "/" + id + "/main.js", "var loaded = true;"));
}

// Writes a file, making its folder.
bool put(const QString &path, const QByteArray &bytes) {
    return QDir().mkpath(QFileInfo(path).absolutePath()) && Fixtures::write(path, bytes);
}

QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class PluginFilesTests : public QObject {
    Q_OBJECT

private slots:
    // Bytes in, as an ArrayBuffer, a typed array or base64, and back out the
    // same; text as before; and nowhere but under data/, within the limits.
    void bytesAreKeptUnderDataAsTheyAre() {
        Bench bench;
        const QString folder = bench.plugin("keeper", keeper, {"files"});
        ScriptPlugin plugin(ScriptManifest::read(folder), bench.services());
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        const QByteArray png = Fixtures::picture(3, 2, Qt::red);
        const QJsonArray answer = plugin.callNow("roundTrip", {QString::fromLatin1(png.toBase64())}, &problem).toArray();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(answer, (QJsonArray{true, 344, 128, "plain words", true}));
        QCOMPARE(contents(folder + "/data/pictures/dot.png"), png);
        QCOMPARE(contents(folder + "/data/half.bin").size(), 128);

        const QJsonObject usage = plugin.callNow("usage", {}, &problem).toObject();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(usage.value("file").toInteger(), PluginFiles::maxFileBytes);
        QCOMPARE(usage.value("plugin").toInteger(), PluginFiles::maxPluginBytes);
        QCOMPARE(usage.value("used").toInteger(), qint64{256 + 128 + png.size() + 11});

        for (const char *refused : {"outside", "notData", "tooLarge", "notBase64"}) {
            problem.clear();
            std::ignore = plugin.callNow(refused, {}, &problem); // only why it threw matters
            QVERIFY2(!problem.isEmpty(), refused);
        }
        QVERIFY(!QFileInfo::exists(bench.dir.filePath("plugins/escape.bin")));
        QVERIFY(!QFileInfo::exists(folder + "/data/big.bin"));
        QVERIFY(QFileInfo(folder + "/main.js").size() > 1);
        problem.clear();
        // A file it removes is removed on one's other computers too: the log says so.
        QVERIFY(plugin.callNow("removed", {}, &problem).toBool());
        const PluginFiles::Files known = PluginFiles::recorded(bench.library, "keeper");
        QVERIFY(known.contains("all.bin") && known.value("all.bin").removed);
    }

    // The person is asked for a file only right after they did something of
    // the plugin's; what they picked is read through its handle.
    void aFileIsPickedOnlyRightAfterThePersonActed() {
        Bench bench;
        const QString picture = bench.dir.filePath("Holiday.png");
        QVERIFY(Fixtures::write(picture, Fixtures::picture(4, 3, Qt::blue)));
        bench.desk.files = {picture};
        const QString folder = bench.plugin("picker", picker, {"files"});
        ScriptPlugin plugin(ScriptManifest::read(folder), bench.services());
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));

        const Answer picked = called(plugin, "pick", {QJsonValue(QJsonArray{"image", ".png"})});
        QVERIFY2(picked.problem.isEmpty(), qPrintable(picked.problem));
        const qint64 size = QFileInfo(picture).size();
        QCOMPARE(picked.result.toArray(), (QJsonArray{"Holiday.png", size, "image/png", 4, size, true}));
        QCOMPARE(bench.desk.kinds, (QStringList{"image", ".png"}));
        QCOMPARE(contents(folder + "/data/in/Holiday.png"), contents(picture));

        // Several, read as text.
        const QString note = bench.dir.filePath("note.txt");
        QVERIFY(Fixtures::write(note, "a note"));
        bench.desk.files = {note, picture};
        QCOMPARE(called(plugin, "pickMany").result.toArray().first().toString(), QString("a note"));

        // Not when the person did nothing a moment ago.
        bench.desk.acted = false;
        QVERIFY(called(plugin, "pick").problem.contains("right after the person did something of the plugin's"));
        // Nor when the plugin wasn't entered by them: an event, though they just acted elsewhere.
        bench.desk.acted = true;
        ScriptPlugin other(ScriptManifest::read(folder), bench.services());
        QVERIFY2(other.load(&problem), qPrintable(problem));
        other.emitEvent("book.added", {});
        QCOMPARE(other.callNow("said", {}, &problem).toString(), QString("refused"));

        // Where there is no window, nothing is picked.
        bench.withDesk = false;
        ScriptPlugin headless(ScriptManifest::read(folder), bench.services());
        QVERIFY2(headless.load(&problem), qPrintable(problem));
        const Answer none = called(headless, "pick");
        QVERIFY2(none.problem.isEmpty(), qPrintable(none.problem));
        QVERIFY(none.result.isNull());
    }

    // Text and pictures go to the clipboard and come from it, with the
    // permission and right after the person acted.
    void theClipboardTakesTextAndPictures() {
        Bench bench;
        const QString folder = bench.plugin("picker", picker, {"files", "clipboard"});
        ScriptPlugin plugin(ScriptManifest::read(folder), bench.services());
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        QVERIFY(plugin.callNow("copy", {"copied words"}, &problem).toBool());
        QCOMPARE(bench.desk.text, QString("copied words"));
        QCOMPARE(plugin.callNow("paste", {}, &problem).toString(), QString("copied words"));

        bench.desk.image = QImage(5, 2, QImage::Format_RGB32);
        bench.desk.image.fill(Qt::green);
        QCOMPARE(plugin.callNow("pastePicture", {}, &problem).toArray(), (QJsonArray{"image/png", 5, 2, true}));
        QCOMPARE(QImage(folder + "/data/pasted.png").size(), QSize(5, 2));
        bench.desk.image = {};
        QVERIFY(plugin.callNow("pastePicture", {}, &problem).isNull());
        QVERIFY2(plugin.callNow("copyPicture", {}, &problem).toBool(), qPrintable(problem));
        QCOMPARE(bench.desk.image.size(), QSize(5, 2));
        bench.desk.image = {};
        QVERIFY2(plugin.callNow("copyPicture", {"data/pasted.png"}, &problem).toBool(), qPrintable(problem));
        QCOMPARE(bench.desk.image.size(), QSize(5, 2));

        bench.desk.acted = false;
        problem.clear();
        std::ignore = plugin.callNow("paste", {}, &problem); // only why it threw matters
        QVERIFY(problem.contains("right after the person did something of the plugin's"));
    }

    // What travels of a data folder: within the limits, never settings.json,
    // nor what a link points to.
    void aDataFolderTravelsWithinItsLimits() {
        QTemporaryDir dir;
        const QString data = dir.filePath("data");
        QVERIFY(put(data + "/pages/one.json", "{}"));
        QVERIFY(put(data + "/settings.json", "{}"));
        QVERIFY(put(dir.filePath("secret.txt"), "not the plugin's"));
        QVERIFY(QFile::link(dir.filePath("secret.txt"), data + "/linked.txt"));
        QFile big(data + "/big.opus");
        QVERIFY(big.open(QIODevice::WriteOnly) && big.resize(PluginFiles::maxFileBytes + 1));
        big.close();
        const PluginFiles::Survey survey = PluginFiles::survey(data);
        QCOMPARE(survey.files.keys(), QStringList{"pages/one.json"});
        QCOMPARE(survey.held, QStringList{"big.opus"});
        QCOMPARE(survey.files.value("pages/one.json").hash, Library::hashFile(data + "/pages/one.json"));
        QVERIFY(!PluginFiles::travels("../out.txt") && !PluginFiles::travels("/etc/passwd") && !PluginFiles::travels("settings.json"));
        QVERIFY(PluginFiles::inside(data, "linked.txt/x").isEmpty());
    }

    // Two libraries, one on each computer: each plugin's data files go where
    // the plugin is installed and they are not, once; a change goes again,
    // and a removal removes it there.
    void dataFilesTravelToTheOtherComputer() {
        QTemporaryDir dir;
        const QString mine = dir.filePath("plugins");
        const QString theirs = dir.filePath("there-home/.config/omabook/plugins");
        installed(mine, "notes");
        installed(mine, "only-here");
        installed(theirs, "notes");
        const QByteArray picture = Fixtures::picture(6, 4, Qt::red);
        QVERIFY(put(mine + "/notes/data/pictures/a.png", picture));
        QVERIFY(put(mine + "/notes/data/settings.json", R"({"here": true})"));
        QVERIFY(put(mine + "/only-here/data/x.txt", "only here"));
        QVERIFY(put(theirs + "/notes/data/sounds/b.opus", "a recording"));
        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        {
            Library there(dir.filePath("There"));
            QVERIFY(there.open());
        }
        const Remote remote = otherComputer(dir, dir.filePath("There"));
        SyncOptions options;
        options.plugins = mine;
        const auto sync = [&] {
            const SyncSummary summary = syncLibrary(here, remote, {}, options);
            if (!summary.error.isEmpty() || !summary.problems.isEmpty())
                qWarning().noquote() << summary.error << summary.problems.join('\n');
            return summary;
        };

        SyncSummary summary = sync();
        QCOMPARE(summary.pluginFilesTaken, 1);
        QCOMPARE(summary.pluginFilesSent, 1);
        QCOMPARE(contents(mine + "/notes/data/sounds/b.opus"), QByteArray("a recording"));
        QCOMPARE(contents(theirs + "/notes/data/pictures/a.png"), picture);
        QVERIFY(!QFileInfo::exists(theirs + "/notes/data/settings.json"));
        QVERIFY(!QFileInfo::exists(theirs + "/only-here"));

        // Sent once: a sync after it carries nothing.
        summary = sync();
        QCOMPARE(summary.pluginFilesTaken + summary.pluginFilesSent, 0);

        // What changed goes again.
        const QByteArray changed = Fixtures::picture(6, 4, Qt::blue);
        QVERIFY(put(mine + "/notes/data/pictures/a.png", changed));
        summary = sync();
        QCOMPARE(summary.pluginFilesSent, 1);
        QCOMPARE(contents(theirs + "/notes/data/pictures/a.png"), changed);

        // Removed here as the plugin does it, removed there, and not fetched back.
        QVERIFY(PluginFiles::forget(here, "notes", "pictures/a.png", Library::hashFile(mine + "/notes/data/pictures/a.png")));
        QVERIFY(QFile::remove(mine + "/notes/data/pictures/a.png"));
        summary = sync();
        QCOMPARE(summary.pluginFilesTaken, 0);
        QVERIFY(!QFileInfo::exists(theirs + "/notes/data/pictures/a.png"));
        QVERIFY(!QFileInfo::exists(mine + "/notes/data/pictures/a.png"));
        QVERIFY(QFileInfo::exists(theirs + "/notes/data/sounds/b.opus"));
    }
};

QTEST_MAIN(PluginFilesTests)
#include "pluginfiles_tests.moc"
