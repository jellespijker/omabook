// A capability's model missing on its server, found or downloaded (#172):
// against a fake Ollama (its /api/tags, /api/show and /api/pull, with and
// without a model that fits) and a fake OpenAI-compatible server, for every
// capability; a plugin told what to do and doing it (`ai.status`'s `remedy`,
// `ai.remedy`); and Find's pane, where indexing a book by its meaning failed
// for want of nomic-embed-text, saying why and downloading it, then indexing,
// walked by the layout survey in English, German and right to left.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

#include "aicapability.h"
#include "aiprivacy.h"
#include "airegistry.h"
#include "desk.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "library.h"
#include "modelfit.h"
#include "modelhelp.h"
#include "scriptplugin.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

// Ollama on this computer, as far as Omabook asks it: what it has, what each
// does, and pulling one more, a line of progress at a time. `ollama` false
// makes it a server of the OpenAI-compatible API alone.
struct FakeOllama {
    struct Held {
        QString name, family;
        QStringList families, abilities;
        qint64 bytes = 1000;
    };
    FakeServer server;
    QList<Held> models;
    bool ollama = true;
    QString pullError; // what a pull answers instead of the model
    QStringList pulled;

    FakeOllama() {
        server.answering = [this](const FakeServer::Request &request) { return answer(request); };
    }

    [[nodiscard]] QString url() const { return server.url(); }
    [[nodiscard]] bool has(const QString &name) const {
        return std::ranges::any_of(models, [&name](const Held &one) { return ModelFit::same(one.name, name); });
    }
    [[nodiscard]] QStringList asked() const {
        QStringList lines;
        for (const auto &request : server.received)
            lines << QString::fromUtf8(request.line.split(' ').value(1));
        return lines;
    }

    static FakeServer::Reply json(const QJsonObject &body, const QByteArray &status = "200 OK") {
        return {.status = status, .body = QJsonDocument(body).toJson(QJsonDocument::Compact)};
    }

    FakeServer::Reply tags() const {
        QJsonArray listed;
        for (const Held &one : std::as_const(models))
            listed.append(
                QJsonObject{{"name", one.name},
                            {"size", one.bytes},
                            {"details", QJsonObject{{"family", one.family}, {"families", QJsonArray::fromStringList(one.families)}}}});
        return json({{"models", listed}});
    }
    FakeServer::Reply show(const QString &name) const {
        const auto found = std::ranges::find_if(models, [&name](const Held &one) { return one.name == name; });
        if (found == models.end())
            return json({{"error", "not found"}}, "404 Not Found");
        return json({{"capabilities", QJsonArray::fromStringList(found->abilities)}, {"details", QJsonObject{{"family", found->family}}}});
    }
    FakeServer::Reply pull(const QString &name) {
        QByteArray lines = R"({"status":"pulling manifest"})"
                           "\n";
        if (!pullError.isEmpty())
            return {.status = "200 OK",
                    .body = lines + QJsonDocument(QJsonObject{{"error", pullError}}).toJson(QJsonDocument::Compact) + "\n"};
        lines += R"({"status":"pulling 970aa74c","digest":"sha256:970aa74c","total":1000,"completed":500})"
                 "\n"
                 R"({"status":"pulling 970aa74c","digest":"sha256:970aa74c","total":1000,"completed":1000})"
                 "\n"
                 R"({"status":"verifying sha256 digest"})"
                 "\n"
                 R"({"status":"success"})"
                 "\n";
        pulled << name;
        models.append({.name = name + (name.contains(':') ? "" : ":latest"),
                       .family = "nomic-bert",
                       .families = {},
                       .abilities = {"embedding", "completion"}});
        return {.status = "200 OK", .body = lines};
    }
    FakeServer::Reply openAiModels() const {
        QJsonArray data;
        for (const Held &one : std::as_const(models))
            data.append(QJsonObject{{"id", one.name}});
        return json({{"data", data}});
    }
    FakeServer::Reply embeddings(const QJsonObject &asked) const {
        const QString model = asked.value("model").toString();
        if (!has(model))
            return json({{"error", QJsonObject{{"message", u"model \"%1\" not found, try pulling it first"_s.arg(model)}}}},
                        "404 Not Found");
        QJsonArray data;
        const QJsonArray input = asked.value("input").toArray();
        for (qsizetype at = 0; at < input.size(); ++at)
            data.append(QJsonObject{{"index", at}, {"embedding", QJsonArray{3.0, 4.0}}});
        return json({{"data", data}});
    }

    FakeServer::Reply answer(const FakeServer::Request &request) {
        const QByteArray path = request.line.split(' ').value(1);
        const QJsonObject asked = request.json();
        if (ollama && path == "/api/tags")
            return tags();
        if (ollama && path == "/api/show")
            return show(asked.value("model").toString());
        if (ollama && path == "/api/pull")
            return pull(asked.value("model").toString());
        if (path == "/v1/models")
            return openAiModels();
        if (path == "/v1/embeddings")
            return embeddings(asked);
        if (path == "/v1/chat/completions") // a model that doesn't read pictures, as Ollama says it
            return json({{"error", QJsonObject{{"message", u"\"%1\" does not support images"_s.arg(asked.value("model").toString())}}}},
                        "400 Bad Request");
        return json({{"error", "no such thing"}}, "404 Not Found");
    }
};

// The settings, each capability going to the fake server as its own (127.0.0.1).
void pointAt(Settings &settings, const QString &url) {
    settings.setCapabilities(Capabilities::fresh());
    settings.setAiEndpoint({{"preset", "custom"}, {"base_url", url}});
    settings.setAiModelFor(AiEndpoints::modelsKey(settings), u"main"_s, u"llama-gone"_s);
    settings.setAiModelFor(AiEndpoints::modelsKey(settings), u"quick"_s, u"llama-gone-small"_s);
    QJsonObject capabilities = settings.capabilities();
    QJsonObject speech = capabilities.value("speech-to-text").toObject();
    speech.insert("provider", "server");
    capabilities.insert("speech-to-text", speech);
    settings.setCapabilities(capabilities);
    settings.setReadAloud({{"engine", "server"}});
}

// Waits for the help to get past looking (or downloading).
[[nodiscard]] bool settled(const ModelHelp &help) {
    return QTest::qWaitFor([&help] { return help.state() != "looking" && help.state() != "downloading"; }, 5000);
}

// A model fit for each capability, and one that isn't, as Ollama describes them.
[[nodiscard]] FakeOllama::Held fitFor(Capability capability) {
    switch (capability) {
    case Capability::Embeddings:
        return {.name = "mxbai-embed-large:latest", .family = "bert", .families = {"bert"}, .abilities = {"embedding"}};
    case Capability::Handwriting:
        return {.name = "gemma4:e2b", .family = "gemma4", .families = {"gemma4"}, .abilities = {"completion", "vision"}};
    case Capability::SpeechToText:
        return {.name = "Systran/faster-whisper-small", .family = {}, .families = {}, .abilities = {}};
    case Capability::TextToSpeech:
        return {.name = "kokoro", .family = {}, .families = {}, .abilities = {}};
    case Capability::Answers:
    case Capability::Quick:
        break;
    }
    return {.name = "qwen3.8:27b", .family = "qwen3", .families = {"qwen3"}, .abilities = {"completion", "thinking"}};
}
[[nodiscard]] FakeOllama::Held unfitFor(Capability capability) {
    if (capability == Capability::Answers || capability == Capability::Quick)
        return {.name = "nomic-embed-text:latest", .family = "nomic-bert", .families = {"nomic-bert"}, .abilities = {"embedding"}};
    // A model that answers, and doesn't read pictures, nor embed, nor hear.
    return {.name = "qwen3.8:27b", .family = "qwen3", .families = {"qwen3"}, .abilities = {"completion", "thinking"}};
}

constexpr const char *notebook = R"(
function status() { return omabook.ai.status({capability: "handwriting", hosted: false}); }
function read() { return omabook.recognize.ink([{points: [{x: 0, y: 0}, {x: 40, y: 30}]}], {hosted: false}); }
var told = [];
function mend() { return omabook.ai.remedy({capability: "handwriting", onProgress: function (done) { told.push(done); }}); }
)";

struct Answer {
    QJsonValue result;
    QString problem;
};
Answer called(ScriptPlugin &plugin, const QString &function) {
    std::optional<Answer> answer;
    plugin.callFunction(function, {}, [&answer](const QJsonValue &result, const QString &problem) { answer = Answer{result, problem}; });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 10000))
        return {{}, u"no answer"_s};
    return *answer;
}

} // namespace

class ModelHelpTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        Secrets::useInstead(&m_secrets);
    }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    // What a model is fit for, by what Ollama says it does, else by its family and name.
    void aModelIsFitByWhatItDoesElseByItsFamily() {
        using ModelFit::Fit;
        const ModelFit::Model gemma{
            .name = "gemma4:e2b", .family = "gemma4", .families = {}, .abilities = {"completion", "vision"}, .bytes = 0};
        const ModelFit::Model qwen{.name = "qwen3.8:27b", .family = "qwen3", .families = {}, .abilities = {"completion"}, .bytes = 0};
        QCOMPARE(ModelFit::fit(Capability::Handwriting, gemma), Fit::Fits);
        QCOMPARE(ModelFit::fit(Capability::Handwriting, qwen), Fit::Unfit);
        QCOMPARE(ModelFit::fit(Capability::Answers, qwen), Fit::Fits);
        QCOMPARE(ModelFit::fit(Capability::Embeddings, qwen), Fit::Unfit);
        // An older Ollama says nothing of what each does: llava's family has CLIP in it.
        const ModelFit::Model llava{.name = "llava:7b", .family = "llama", .families = {"llama", "clip"}, .abilities = {}, .bytes = 0};
        QCOMPARE(ModelFit::fit(Capability::Handwriting, llava), Fit::Fits);
        const ModelFit::Model plain{.name = "mistral:7b", .family = "llama", .families = {"llama"}, .abilities = {}, .bytes = 0};
        QCOMPARE(ModelFit::fit(Capability::Handwriting, plain), Fit::Unknown);
        const ModelFit::Model nomic{.name = "nomic-embed-text:latest", .family = "nomic-bert", .families = {}, .abilities = {}, .bytes = 0};
        QCOMPARE(ModelFit::fit(Capability::Embeddings, nomic), Fit::Fits);
        QCOMPARE(ModelFit::fit(Capability::Answers, nomic), Fit::Unfit);
        QCOMPARE(ModelFit::fit(Capability::SpeechToText,
                               {.name = "whisper-large-v3", .family = {}, .families = {}, .abilities = {}, .bytes = 0}),
                 Fit::Fits);
        QVERIFY(ModelFit::same("nomic-embed-text", "nomic-embed-text:latest"));
        QVERIFY(!ModelFit::same("nomic-embed-text", "nomic-embed-text:v1.5"));
    }

    void aFittingModelOnTheServerIsOfferedAndChosen_data() {
        QTest::addColumn<int>("capability");
        for (const Capability capability : Capabilities::all)
            QTest::newRow(qPrintable(Capabilities::name(capability))) << static_cast<int>(capability);
    }
    // For each capability: the model asked for isn't there (or can't do it), one
    // that can is, and it is offered, then chosen where its provider keeps it.
    void aFittingModelOnTheServerIsOfferedAndChosen() {
        QFETCH(int, capability);
        const auto which = static_cast<Capability>(capability);
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        FakeOllama ollama;
        // Speech either way goes to a server of the OpenAI-compatible API, never Ollama.
        ollama.ollama = which != Capability::SpeechToText && which != Capability::TextToSpeech;
        ollama.models = {unfitFor(which), fitFor(which)};
        pointAt(settings, ollama.url());
        // The real case: handwriting asked of the main model, which doesn't read pictures.
        if (which == Capability::Handwriting)
            settings.setRecognition({{"ink_model", "qwen3.8:27b"}});
        ModelHelp help(&settings, which);
        help.look();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("offer"));
        QCOMPARE(help.model(), fitFor(which).name);
        QCOMPARE(help.unfit(), which == Capability::Handwriting);
        QCOMPARE(help.remedy().value("action").toString(), QString("use"));
        QSignalSpy chosen(&help, &ModelHelp::chosen);
        help.use();
        QCOMPARE(help.state(), QString("chosen"));
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(ModelFit::placeOf(settings, which)->model, fitFor(which).name);
        // Looked at again, it is there now: nothing to mend.
        help.look();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("there"));
        QVERIFY(ollama.pulled.isEmpty());
    }

    void withoutOneASmallOneIsDownloadedWithItsProgress_data() {
        QTest::addColumn<int>("capability");
        for (const Capability capability : {Capability::Answers, Capability::Quick, Capability::Handwriting, Capability::Embeddings})
            QTest::newRow(qPrintable(Capabilities::name(capability))) << static_cast<int>(capability);
    }
    // Nothing on the Ollama server can do it: a small, well-known model is
    // suggested with its size, downloaded with a press, its progress told, and chosen.
    void withoutOneASmallOneIsDownloadedWithItsProgress() {
        QFETCH(int, capability);
        const auto which = static_cast<Capability>(capability);
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        FakeOllama ollama;
        ollama.models = {unfitFor(which)};
        pointAt(settings, ollama.url());
        ModelHelp help(&settings, which);
        help.look();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("suggest"));
        const auto suggestion = ModelFit::suggested(which);
        QVERIFY(suggestion);
        QCOMPARE(help.model(), suggestion->name);
        QVERIFY2(!help.size().isEmpty() && help.size().contains("B"), qPrintable(help.size()));
        QCOMPARE(help.remedy().value("action").toString(), QString("download"));
        QList<double> progress;
        connect(&help, &ModelHelp::changed, &help, [&] {
            if (help.state() == "downloading")
                progress << help.progress();
        });
        help.download();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("chosen"));
        QCOMPARE(ollama.pulled, QStringList{suggestion->name});
        QVERIFY2(progress.contains(0.5) && progress.contains(1.0), qPrintable(QString::number(progress.size())));
        QCOMPARE(ModelFit::placeOf(settings, which)->model, suggestion->name);
        QVERIFY(ollama.asked().contains("/api/pull"));
    }

    // Handwriting's own server (#170) is the one looked at and sent the
    // download, not the AI server, which has a model that reads pictures.
    void handwritingsOwnServerIsTheOneAsked() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        FakeOllama aiServer, inkServer;
        aiServer.models = {fitFor(Capability::Handwriting)};
        inkServer.models = {unfitFor(Capability::Handwriting)};
        pointAt(settings, aiServer.url());
        settings.setRecognition({{"ink_url", inkServer.url()}});
        ModelHelp help(&settings, Capability::Handwriting);
        help.look();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("suggest"));
        QCOMPARE(help.server(), QUrl(inkServer.url()).authority());
        QCOMPARE(help.model(), ModelFit::suggested(Capability::Handwriting)->name);
        help.download();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("chosen"));
        QCOMPARE(inkServer.pulled, QStringList{help.model()});
        QVERIFY(aiServer.server.received.isEmpty());
        QCOMPARE(settings.recognition().value("ink_model").toString(), help.model());
        QCOMPARE(settings.recognition().value("ink_url").toString(), inkServer.url());
    }

    // Speech either way has nothing to download: a server without a model for it says so.
    void aServerWithNothingForSpeechSaysSo() {
        for (const Capability which : {Capability::SpeechToText, Capability::TextToSpeech}) {
            QTemporaryDir dir;
            Settings settings(dir.filePath("config.json"));
            FakeOllama ollama;
            ollama.ollama = false;
            ollama.models = {unfitFor(which)};
            pointAt(settings, ollama.url());
            ModelHelp help(&settings, which);
            help.look();
            QVERIFY(settled(help));
            QCOMPARE(help.state(), QString("missing"));
            QVERIFY(help.remedy().isEmpty());
        }
    }

    // A pull Ollama refuses says why, and may be tried again; one stopped is offered again.
    void aPullThatFailsSaysWhy() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        FakeOllama ollama;
        ollama.pullError = "pull model manifest: file does not exist";
        pointAt(settings, ollama.url());
        ModelHelp help(&settings, Capability::Embeddings);
        help.look();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("suggest"));
        help.download();
        QVERIFY(settled(help));
        QCOMPARE(help.state(), QString("failed"));
        QVERIFY2(help.problem().contains("file does not exist") && help.problem().contains("nomic-embed-text"), qPrintable(help.problem()));
        QCOMPARE(settings.meaning().value("model").toString(), QString());
        ollama.pullError.clear();
        help.download();
        help.cancel();
        QCOMPARE(help.state(), QString("suggest"));
    }

    // With the AI Off nothing is asked of any server.
    void withTheAiOffNothingIsAsked() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        FakeOllama ollama;
        pointAt(settings, ollama.url());
        settings.setAiProvider("off");
        for (const Capability which : Capabilities::all) {
            ModelHelp help(&settings, which);
            help.look();
            QCOMPARE(help.state(), QString("none"));
        }
        QTest::qWait(50);
        QVERIFY(ollama.server.received.isEmpty());
    }

    // The Notebooks plugin's handwriting: the server's main model doesn't read
    // pictures, which `ai.status` then says, with what can be done about it;
    // `ai.remedy` does it, and the ink is read by the model chosen.
    void aPluginIsToldWhatToDoAndDoesIt() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        AiPrivacy::carryOver(settings);
        FakeOllama ollama;
        ollama.models = {unfitFor(Capability::Handwriting), fitFor(Capability::Handwriting)};
        pointAt(settings, ollama.url());
        settings.setAiModelFor(AiEndpoints::modelsKey(settings), u"main"_s, u"qwen3.8:27b"_s);
        const QString folder = dir.filePath("plugins/notebooks");
        QVERIFY(QDir().mkpath(folder + "/data"));
        QVERIFY(Fixtures::write(folder + "/plugin.json",
                                QJsonDocument(QJsonObject{{"name", "Notebooks"}, {"version", "1.0"}, {"script", "main.js"}}).toJson()));
        QVERIFY(Fixtures::write(folder + "/main.js", notebook));
        ScriptServices services;
        services.settings = &settings;
        services.library = [&library] { return &library; };
        services.ai = AiRegistry::answerer(settings);
        ScriptPlugin plugin(ScriptManifest::read(folder), services);
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));

        const Answer read = called(plugin, "read");
        QVERIFY2(read.problem.contains("qwen3.8:27b"), qPrintable(read.problem));
        QJsonObject status = called(plugin, "status").result.toObject();
        QCOMPARE(status.value("why").toString(), QString("no-model"));
        // The server is looked at the first time, and what can be done is said from then on.
        QTRY_COMPARE(called(plugin, "status").result.toObject().value("remedy").toObject().value("model").toString(),
                     QString("gemma4:e2b"));
        status = called(plugin, "status").result.toObject();
        QCOMPARE(status.value("remedy").toObject().value("action").toString(), QString("use"));
        QCOMPARE(status.value("remedy").toObject().value("server").toString(), QUrl(ollama.url()).authority());

        const Answer mended = called(plugin, "mend");
        QVERIFY2(mended.problem.isEmpty(), qPrintable(mended.problem));
        QCOMPARE(mended.result.toString(), QString("gemma4:e2b"));
        QCOMPARE(settings.recognition().value("ink_model").toString(), QString("gemma4:e2b"));
        // The ways are the new model's: it isn't taken for lacking one.
        QCOMPARE(called(plugin, "status").result.toObject().value("why").toString(), QString());
    }

    // Find's pane, where Index did nothing visible (#172): the press shows at
    // once, the reason is said where it was pressed, nomic-embed-text is
    // offered for download onto the Ollama server, and once it is there the
    // book is indexed. In English, German and right to left, walked by the survey.
    void findsPaneSaysWhyAndDownloadsTheModel() {
        for (const QByteArray language : {QByteArray("en"), QByteArray("de"), QByteArray("ar")}) {
            Desk desk(language, language == "ar" ? lightTheme : QByteArray(), false);
            FakeOllama ollama;
            ollama.models = {fitFor(Capability::Handwriting)};
            pointAt(*desk.settings, ollama.url());
            const QString epub = desk.dir.filePath("ferry.epub");
            QVERIFY(Fixtures::chapteredEpub(epub, "The Ferry",
                                            {{"One", "<h1>One</h1><p>The ferry left the harbour at dawn, its lamps still lit.</p>"},
                                             {"Two", "<h1>Two</h1><p>Gulls followed it out past the breakwater.</p>"}}));
            const qint64 book = desk.backend->library()->add(epub).id;
            QVERIFY(book > 0);
            desk.backend->reload();
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            desk.reader->openBook(book);
            QTRY_VERIFY(desk.reader->isOpen());
            QVERIFY(desk.run("reader.find"));
            QVERIFY(desk.waitForPopup());
            QTRY_VERIFY(desk.shows("findMeaningIndex"));

            QVERIFY(QMetaObject::invokeMethod(desk.find("findMeaningIndex"), "clicked"));
            QTRY_VERIFY(desk.shows("findMeaningWhy"));
            const QString said = desk.find("findMeaningProblem")->property("text").toString();
            QVERIFY2(said.contains("nomic-embed-text"), qPrintable(said));
            QTRY_VERIFY2(desk.shows("findModelDownload"), qPrintable(ollama.asked().join(' ') + " | " + desk.warnings.join('\n')));
            QVERIFY2(desk.find("findModelSays")->property("text").toString().contains("nomic-embed-text"),
                     qPrintable(desk.find("findModelSays")->property("text").toString()));
            for (const QSize size : {QSize(420, 700), QSize(1280, 800)}) {
                QVERIFY(desk.resize(size));
                QVERIFY(desk.pictured());
                Survey survey{desk.window, desk.touch, {}};
                survey.walk();
                QVERIFY2(survey.problems.isEmpty(), qPrintable(QString::fromLatin1(language) + ": " + survey.problems.join('\n')));
                desk.shot(QString("%1-find-no-model-%2x%3").arg(QString::fromLatin1(language)).arg(size.width()).arg(size.height()));
            }

            QVERIFY(QMetaObject::invokeMethod(desk.find("findModelDownload"), "clicked"));
            QTRY_VERIFY_WITH_TIMEOUT(desk.shows("findMeaningWhole"), 10000);
            QCOMPARE(ollama.pulled, QStringList{"nomic-embed-text"});
            QCOMPARE(desk.settings->meaning().value("model").toString(), QString("nomic-embed-text"));
            QVERIFY(!desk.shows("findMeaningWhy"));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }
};

QTEST_MAIN(ModelHelpTests)
#include "modelhelp_tests.moc"
