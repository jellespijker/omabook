// What a plugin exports for the person, and asks of the AI (#138): a file
// saved where they choose and nowhere in the plugin's data/; whether the
// model they chose is their own, and asking only their own; a PDF drawn by
// Omabook, read back here with Poppler; and links to a place in a book,
// which the window already open takes.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>
#include <optional>
#include <poppler-qt6.h>
#include <tuple>

#include "ai.h"
#include "booklink.h"
#include "filepicker.h"
#include "fixtures.h"
#include "library.h"
#include "linkinbox.h"
#include "plugindesk.h"
#include "scriptdesk.h"
#include "scriptplugin.h"
#include "settings.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// A desktop that answers at once, as the person would: where they chose to
// save (empty for nowhere), having just acted or not.
class Desk : public ScriptDesk {
public:
    bool acted = true;
    QString target;
    QString named;
    QStringList kinds;

    [[nodiscard]] bool justActed() const override { return acted; }
    void pickFiles(const QStringList &, bool, const std::function<void(const QStringList &)> &picked) override { picked({}); }
    void saveFile(const QString &name, const QStringList &asked, const std::function<void(const QString &)> &chosen) override {
        named = name;
        kinds = asked;
        QTimer::singleShot(0, [chosen, where = target] { chosen(where); });
    }
    [[nodiscard]] QString clipboardText() const override { return {}; }
    void setClipboardText(const QString &) override {}
    [[nodiscard]] QImage clipboardImage() const override { return {}; }
    void setClipboardImage(const QImage &) override {}
};

// A model that answers, and counts how often it was asked.
class CountingAi : public AiProvider {
public:
    explicit CountingAi(int &asked) : m_asked(asked) {}
    QString name() const override { return "Counter"; }
    void ask(const AiRequest &) override {
        ++m_asked;
        QTimer::singleShot(0, this, [this] {
            emit delta("answered");
            emit finished();
        });
    }
    void cancel() override {}

private:
    int &m_asked;
};

struct Bench {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    Library library{dir.filePath("Library")};
    Desk desk;
    bool withDesk = true;
    int asked = 0;
    // The model the settings choose, as the window makes it; a counting one when not.
    bool realAi = false;

    Bench() {
        library.open();
        settings.setAiProvider("openai");
    }
    void serve(const QString &url) { settings.setAiEndpoint({{"preset", "custom"}, {"base_url", url}}); }

    std::unique_ptr<ScriptPlugin> plugin(const QByteArray &script, const QStringList &permissions = {"files"}) {
        const QString folder = dir.filePath("plugins/notes");
        QDir().mkpath(folder + "/data");
        const QJsonObject manifest{
            {"name", "Notes"}, {"version", "1.0"}, {"script", "main.js"}, {"permissions", QJsonArray::fromStringList(permissions)}};
        Fixtures::write(folder + "/plugin.json", QJsonDocument(manifest).toJson());
        Fixtures::write(folder + "/main.js", script);
        ScriptServices services;
        services.settings = &settings;
        services.library = [this] { return &library; };
        services.desk = withDesk ? &desk : nullptr;
        if (realAi)
            services.ai = [this](QString *why) { return makeAiProvider(settings, why); };
        else
            services.ai = [this](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<CountingAi>(asked); };
        auto made = std::make_unique<ScriptPlugin>(ScriptManifest::read(folder), services);
        QString problem;
        [&] { QVERIFY2(made->load(&problem), qPrintable(problem)); }();
        return made;
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

QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

constexpr const char *saver = R"(
function saveText(name) { return omabook.ui.saveFile({name: name, kinds: [".md"], text: "# Monday\n"}); }
function saveBytes() { return omabook.ui.saveFile({name: "dots.bin", bytes: new Uint8Array([1, 2, 3])}); }
function saveNothing() { return omabook.ui.saveFile({name: "empty.txt"}); }
)";

constexpr const char *asker = R"(
function status(options) { return omabook.ai.status(options); }
function ask(options) { return omabook.ai.ask("Summarise my notes.", options); }
)";

constexpr const char *drawer = R"(
function draw(png) {
    var pdf = omabook.pdf.create({width: 400, height: 300, title: "Notebook"});
    var first = pdf.page();
    first.text("Monday notes", {x: 20, y: 20, size: 18, bold: true});
    first.text("in the font of the reader", {x: 20, y: 60, size: 10, font: "reader"});
    first.path([[100, 150], [300, 150]], {width: 20, color: "#ff0000"});
    first.path([{x: 100, y: 220}, {x: 300, y: 220}], {width: 20, color: "#0000ff", alpha: 0.5});
    var second = pdf.page({width: 600, height: 200});
    second.image(new Uint8Array(omabook.files.readBytes("data/dot.png")), {x: 0, y: 0, width: 100});
    second.path([200, 20, 300, 20, 300, 120, 200, 120], {width: 0, fill: "#00ff00", closed: true});
    omabook.files.write("data/notes.pdf", pdf.bytes());
    return pdf.pages();
}
function noPage() { return omabook.pdf.create().bytes(); }
function badPicture() {
    var pdf = omabook.pdf.create();
    pdf.page().image(new Uint8Array([1, 2, 3]), {x: 0, y: 0});
    return pdf.bytes();
}
function noSize() { var pdf = omabook.pdf.create(); pdf.page({width: 0}); return pdf.bytes(); }
)";

// The colour of a page drawn at 72 dots an inch, a dot a point, at a point.
QColor colorAt(const Poppler::Page &page, int x, int y) { return page.renderToImage(72, 72).pixelColor(x, y); }

// A picker that answers as a test says, and remembers what it was asked.
class Picker : public FilePicker {
public:
    QString saveName;
    QStringList saveKinds;
    void pick(Purpose, const QString &) override {}
    void saveForPlugin(const QString &name, const QStringList &kinds) override {
        saveName = name;
        saveKinds = kinds;
    }
};

} // namespace

class PluginExportsTests : public QObject {
    Q_OBJECT

private slots:
    // Saved where the person chose, text or bytes, and nothing of it in data/.
    void aFileIsSavedWhereThePersonChose() {
        Bench bench;
        bench.desk.target = bench.dir.filePath("Documents/Monday.md");
        QDir().mkpath(bench.dir.filePath("Documents"));
        const auto plugin = bench.plugin(saver);
        const Answer saved = called(*plugin, "saveText", {"../Monday.md"});
        QVERIFY2(saved.problem.isEmpty(), qPrintable(saved.problem));
        QCOMPARE(saved.result.toObject(), (QJsonObject{{"name", "Monday.md"}, {"size", 9}}));
        QCOMPARE(contents(bench.desk.target), QByteArray("# Monday\n"));
        // Only the name is the plugin's to suggest, never a folder.
        QCOMPARE(bench.desk.named, QString("Monday.md"));
        QCOMPARE(bench.desk.kinds, QStringList{".md"});
        QVERIFY(QDir(bench.dir.filePath("plugins/notes/data")).isEmpty());

        bench.desk.target = bench.dir.filePath("Documents/dots.bin");
        QVERIFY2(called(*plugin, "saveBytes").problem.isEmpty(), "bytes");
        QCOMPARE(contents(bench.desk.target), QByteArray("\x01\x02\x03"));

        // Dismissed: nothing saved.
        bench.desk.target.clear();
        const Answer none = called(*plugin, "saveBytes");
        QVERIFY2(none.problem.isEmpty(), qPrintable(none.problem));
        QVERIFY(none.result.isNull());
        // Somewhere it can't be written.
        bench.desk.target = bench.dir.filePath("no/such/folder/dots.bin");
        QVERIFY(called(*plugin, "saveBytes").problem.contains("can't be written there"));
        // Nothing to save.
        QVERIFY(called(*plugin, "saveNothing").problem.contains("needs what to save"));
    }

    // Only right after the person did something of the plugin's, with the
    // permission; where there is no window, nothing is saved.
    void aFileIsSavedOnlyOnThePersonsAction() {
        Bench bench;
        bench.desk.target = bench.dir.filePath("dots.bin");
        bench.desk.acted = false;
        const auto plugin = bench.plugin(saver);
        QVERIFY(called(*plugin, "saveBytes").problem.contains("right after the person did something of the plugin's"));
        QVERIFY(!QFileInfo::exists(bench.desk.target));

        bench.withDesk = false;
        const auto headless = bench.plugin(saver);
        const Answer none = called(*headless, "saveBytes");
        QVERIFY2(none.problem.isEmpty(), qPrintable(none.problem));
        QVERIFY(none.result.isNull());

        bench.withDesk = true;
        const auto unpermitted = bench.plugin(saver, {});
        QVERIFY(!called(*unpermitted, "saveBytes").problem.isEmpty());
    }

    // The window's desk asks the picker with the plugin's name and kinds,
    // and a dialog closed without a choice is no file.
    void theWindowsSaveDialogAnswersOnce() {
        Picker picker;
        PluginDesk desk(&picker);
        std::optional<QString> chosen;
        desk.saveFile("Notebook.pdf", {".pdf"}, [&chosen](const QString &path) { chosen = path; });
        QCOMPARE(picker.saveName, QString("Notebook.pdf"));
        QCOMPARE(picker.saveKinds, QStringList{".pdf"});
        // A second while the first is open gets nothing.
        std::optional<QString> second;
        desk.saveFile("Other.pdf", {}, [&second](const QString &path) { second = path; });
        QCOMPARE(second, std::optional<QString>(QString()));
        // Another purpose's answer isn't the plugin's.
        emit picker.picked(FilePicker::ExportBook, {QUrl::fromLocalFile("/tmp/copy.epub")});
        QVERIFY(!chosen);
        emit picker.picked(FilePicker::PluginSave, {QUrl::fromLocalFile("/tmp/Notebook.pdf")});
        QCOMPARE(chosen, std::optional<QString>("/tmp/Notebook.pdf"));

        chosen.reset();
        desk.saveFile("Notebook.pdf", {}, [&chosen](const QString &path) { chosen = path; });
        emit picker.dismissed(FilePicker::PluginSave);
        QCOMPARE(chosen, std::optional<QString>(QString()));

        // A file being picked is answered when its dialog is closed too.
        std::optional<QStringList> picked;
        desk.pickFiles({}, false, [&picked](const QStringList &paths) { picked = paths; });
        emit picker.dismissed(FilePicker::PluginFiles);
        QCOMPARE(picked, std::optional<QStringList>(QStringList()));
    }

    // Whether the chosen model is the person's own; asking with `hosted:
    // false` never reaches anyone else's.
    void theAiSaysWhetherItIsThePersonsOwn() {
        Bench bench;
        bench.serve("http://localhost:11434/v1");
        const auto plugin = bench.plugin(asker, {});
        QString problem;
        QJsonObject status = plugin->callNow("status", {QJsonObject{{"hosted", false}}}, &problem).toObject();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(status.value("available").toBool(), true);
        QCOMPARE(status.value("hosted").toBool(), false);
        QCOMPARE(status.value("why").toString(), QString());
        QCOMPARE(status.value("capability").toString(), QString("answers"));
        QCOMPARE(called(*plugin, "ask", {QJsonObject{{"hosted", false}}}).result.toString(), QString("answered"));
        QCOMPARE(bench.asked, 1);

        // A model that runs elsewhere, though its server is here.
        status = plugin->callNow("status", {QJsonObject{{"model", "gpt-oss:120b-cloud"}, {"hosted", false}}}, &problem).toObject();
        QCOMPARE(status.value("why").toString(), QString("hosted-not-chosen"));

        // Someone else's server: told, refused with hosted: false, asked otherwise.
        bench.serve("https://api.example.com/v1");
        status = plugin->callNow("status", {QJsonObject{{"hosted", false}}}, &problem).toObject();
        QCOMPARE(status.value("available").toBool(), false);
        QCOMPARE(status.value("hosted").toBool(), true);
        QCOMPARE(status.value("why").toString(), QString("hosted-not-chosen"));
        QVERIFY(!status.value("message").toString().isEmpty());
        QVERIFY(called(*plugin, "ask", {QJsonObject{{"hosted", false}}}).problem.contains("someone else's computer"));
        QCOMPARE(bench.asked, 1);
        status = plugin->callNow("status", {}, &problem).toObject();
        QCOMPARE(status.value("available").toBool(), true);
        QCOMPARE(status.value("hosted").toBool(), true);
        QCOMPARE(called(*plugin, "ask", {QJsonObject{{"hosted", true}}}).result.toString(), QString("answered"));
        QCOMPARE(bench.asked, 2);

        // An address the person marked as theirs is their own.
        bench.settings.setRecognition({{"own", QJsonArray{"https://api.example.com"}}});
        QCOMPARE(plugin->callNow("status", {QJsonObject{{"hosted", false}}}, &problem).toObject().value("hosted").toBool(), false);

        // Claude's API is someone else's.
        bench.settings.setAiProvider("anthropic");
        QCOMPARE(plugin->callNow("status", {}, &problem).toObject().value("hosted").toBool(), true);
    }

    // With the AI set to Off nothing is asked, and status says why.
    void nothingIsAskedWithTheAiOff() {
        Bench bench;
        bench.realAi = true;
        bench.settings.setAiProvider("off");
        const auto plugin = bench.plugin(asker, {});
        QString problem;
        const QJsonObject status = plugin->callNow("status", {}, &problem).toObject();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(status.value("available").toBool(), false);
        QCOMPARE(status.value("why").toString(), QString("ai-off"));
        QVERIFY(called(*plugin, "ask").problem.contains("turned off"));
    }

    // Pages of their sizes, text in an embedded font, vectors in their
    // colours and alpha, and a picture: read back as another PDF reader would.
    void aPdfIsDrawnAndReadsBack() {
        Bench bench;
        bench.settings.setFontFamily("DejaVu Serif");
        const auto plugin = bench.plugin(drawer);
        QVERIFY(Fixtures::write(bench.dir.filePath("plugins/notes/data/dot.png"), Fixtures::picture(10, 10, Qt::blue)));
        QString problem;
        QCOMPARE(plugin->callNow("draw", {}, &problem).toInt(), 2);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        const std::unique_ptr<Poppler::Document> pdf =
            Poppler::Document::loadFromData(contents(bench.dir.filePath("plugins/notes/data/notes.pdf")));
        QVERIFY(pdf && !pdf->isLocked());
        QCOMPARE(pdf->numPages(), 2);
        QCOMPARE(pdf->info("Title"), QString("Notebook"));
        const std::unique_ptr<Poppler::Page> first = pdf->page(0);
        const std::unique_ptr<Poppler::Page> second = pdf->page(1);
        QCOMPARE(first->pageSizeF().toSize(), QSize(400, 300));
        QCOMPARE(second->pageSizeF().toSize(), QSize(600, 200));
        QVERIFY2(first->text({}).contains("Monday notes"), qPrintable(first->text({})));
        QVERIFY(first->text({}).contains("in the font of the reader"));
        const QList<Poppler::FontInfo> fonts = pdf->fonts();
        QVERIFY(!fonts.isEmpty());
        QVERIFY(std::ranges::all_of(fonts, &Poppler::FontInfo::isEmbedded));

        // The red line, solid; the blue one at half, over white.
        QCOMPARE(colorAt(*first, 200, 150), QColor(255, 0, 0));
        const QColor blue = colorAt(*first, 200, 220);
        QVERIFY2(blue.blue() > 240 && blue.red() > 110 && blue.red() < 145, qPrintable(blue.name()));
        QCOMPARE(colorAt(*first, 200, 280), QColor(Qt::white));
        // The picture, at a hundred points wide, and the filled shape.
        // (Qt may keep a picture as a JPEG, near enough.)
        const QColor picture = colorAt(*second, 50, 50);
        QVERIFY2(picture.blue() > 240 && picture.red() < 16 && picture.green() < 16, qPrintable(picture.name()));
        QCOMPARE(colorAt(*second, 250, 70), QColor(0, 255, 0));
    }

    // What can't be drawn is said, and nothing is written.
    void aPdfThatCantBeDrawnSaysWhy() {
        Bench bench;
        const auto plugin = bench.plugin(drawer);
        QString problem;
        std::ignore = plugin->callNow("badPicture", {}, &problem); // only why it threw matters
        QVERIFY2(problem.contains("Picture 1 on page 1"), qPrintable(problem));
        problem.clear();
        std::ignore = plugin->callNow("noSize", {}, &problem);
        QVERIFY2(problem.contains("Page 1 is no size"), qPrintable(problem));
        problem.clear();
        std::ignore = plugin->callNow("noPage", {}, &problem);
        QVERIFY2(problem.contains("needs a page"), qPrintable(problem));
    }

    // A link names a book by its global id, at a section or where it was left.
    void aLinkIsReadAndMade() {
        QCOMPARE(BookLink::parse("omabook://book/b:abc123/section/4")->section, 4);
        QCOMPARE(BookLink::parse("omabook://book/b:abc123/section/4")->book, QString("b:abc123"));
        QCOMPARE(BookLink::parse("omabook://book/b:abc123/")->section, -1);
        QCOMPARE(BookLink::parse(" omabook://book/12 ")->book, QString("12"));
        for (const char *wrong :
             {"omabook://shelf/b:abc", "omabook://book/", "omabook://book/b:abc/section/-1", "omabook://book/b:abc/page/2",
              "omabook://book/b:abc/section/two", "https://book/b:abc", "/home/book.epub", ""})
            QVERIFY2(!BookLink::parse(wrong), wrong);
        QCOMPARE(BookLink::make("b:abc123", 4), QString("omabook://book/b:abc123/section/4"));
        QCOMPARE(BookLink::make("e:one two"), QString("omabook://book/e:one%20two"));
        QCOMPARE(BookLink::parse(BookLink::make("e:one two", 0))->book, QString("e:one two"));
    }

    // In a library, by its global id, which a plugin is given, or by its number.
    void aLinkFindsItsBook() {
        Bench bench;
        const QString file = bench.dir.filePath("book.txt");
        QVERIFY(Fixtures::write(file, "A book of a few words."));
        const qint64 id = bench.library.add(file).id;
        QVERIFY(id > 0);
        const QString gid = Spaces::bookId(bench.library, id);
        QVERIFY(!gid.isEmpty());
        QCOMPARE(BookLink::find(bench.library, gid), id);
        QCOMPARE(BookLink::find(bench.library, QString::number(id)), id);
        QCOMPARE(BookLink::find(bench.library, "b:none"), qint64{0});
        QCOMPARE(BookLink::find(bench.library, "999"), qint64{0});

        const auto plugin = bench.plugin(QByteArray("function gid(id) { return omabook.library.get(id).global_id; }"), {});
        QString problem;
        QCOMPARE(plugin->callNow("gid", {id}, &problem).toString(), gid);
    }

    // The first window to listen takes the links; a second doesn't steal
    // them, and one left behind by a window that is gone is taken over.
    void theWindowOpenTakesTheLink() {
        QTemporaryDir dir;
        const QString name = dir.filePath("links");
        QVERIFY(!LinkInbox::handOver("omabook://book/b:abc", name));
        LinkInbox first(name);
        QVERIFY(first.listen());
        LinkInbox second(name);
        QVERIFY(!second.listen());
        QSignalSpy heard(&first, &LinkInbox::linked);
        QVERIFY(LinkInbox::handOver("omabook://book/b:abc/section/2", name));
        QTRY_COMPARE(heard.size(), 1);
        QCOMPARE(heard.first().first().toString(), QString("omabook://book/b:abc/section/2"));

        // A socket file with nobody behind it.
        const QString stale = dir.filePath("stale");
        QVERIFY(Fixtures::write(stale, ""));
        LinkInbox taking(stale);
        QVERIFY(taking.listen());
    }
};

QTEST_MAIN(PluginExportsTests)
#include "pluginexports_tests.moc"
