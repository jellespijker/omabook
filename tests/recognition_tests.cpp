// The recognition service (#84): handwriting read by a vision model, speech
// transcribed with each word's moment (by a transcription server, else
// whisper.cpp), in the background, cancellable, waiting for a server that is
// away, kept in the change log by the hash of the input and the model, only
// ever sent to one's own servers unless a hosted one was chosen twice over,
// and called by a plugin. Every server here is a stand-in on this computer.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <optional>

#include "ai.h"
#include "embeddings.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "inkcodec.h"
#include "inkpicture.h"
#include "library.h"
#include "plugindata.h"
#include "recognition.h"
#include "recognitionqueue.h"
#include "recognitionroute.h"
#include "recognitionstore.h"
#include "recognizers.h"
#include "scriptplugin.h"
#include "secretstore.h"
#include "settings.h"
#include "whisperlocal.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

using Fixtures::whyNot;
using Outcome = std::expected<Recognition::Result, QString>;

// A port nothing listens on, until a server is made on it.
[[nodiscard]] quint16 freePort() {
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost);
    return probe.serverPort();
}

// Writing as a pen draws it: a few strokes, a moment apart.
[[nodiscard]] std::vector<Ink::Stroke> writing(double shift = 0) {
    std::vector<Ink::Stroke> strokes;
    for (int line = 0; line < 3; ++line) {
        Ink::Stroke stroke{.id = Ink::newId(),
                           .color = Qt::blue,
                           .width = 3,
                           .pressured = true,
                           .tilted = false,
                           .began = 1'700'000'000'000 + line * 400,
                           .points = {}};
        for (int i = 0; i < 20; ++i)
            stroke.points.push_back({.at = {10.0 + line * 30 + shift, 10.0 + i * 4},
                                     .pressure = 0.6F,
                                     .xTilt = 0,
                                     .yTilt = 0,
                                     .ms = static_cast<quint32>(i * 8)});
        strokes.push_back(stroke);
    }
    return strokes;
}

[[nodiscard]] Recognition::Input ink(std::vector<Ink::Stroke> strokes = writing()) {
    return {.kind = Recognition::Kind::Ink, .strokes = std::move(strokes), .file = {}, .language = {}};
}

// What a whisper server answers: seconds, each word apart from the segments.
[[nodiscard]] QJsonObject verbose() {
    return {{"text", " Hello there. How are you?"},
            {"language", "english"},
            {"segments", QJsonArray{QJsonObject{{"start", 0.0}, {"end", 1.2}, {"text", " Hello there."}},
                                    QJsonObject{{"start", 1.5}, {"end", 2.9}, {"text", " How are you?"}}}},
            {"words", QJsonArray{QJsonObject{{"word", "Hello"}, {"start", 0.0}, {"end", 0.5}},
                                 QJsonObject{{"word", "there."}, {"start", 0.6}, {"end", 1.2}},
                                 QJsonObject{{"word", "How"}, {"start", 1.5}, {"end", 1.8}},
                                 QJsonObject{{"word", "are"}, {"start", 1.9}, {"end", 2.2}},
                                 QJsonObject{{"word", "you?"}, {"start", 2.3}, {"end", 2.9}}}}};
}

// A computer with its settings, its library and the queue, the AI server
// being a stand-in here (this computer: always one's own).
struct Desk {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    Library library{dir.filePath("Library")};
    std::unique_ptr<RecognitionQueue> queue;

    Desk() {
        library.open();
        settings.setAiProvider("openai");
        queue = std::make_unique<RecognitionQueue>(RecognitionQueue::routeFor(settings), [this] { return &library; });
    }
    void serve(const QString &url) { settings.setAiEndpoint({{"preset", "custom"}, {"base_url", url}}); }
    void choose(const QJsonObject &recognition) { settings.setRecognition(recognition); }

    // Recognises, and waits for what comes of it.
    Outcome recognise(Recognition::Input input, bool again = false, bool hosted = false) {
        std::optional<Outcome> outcome;
        queue->submit({.input = std::move(input), .owner = u"notebook"_s, .hosted = hosted, .again = again},
                      [&outcome](const Outcome &done) { outcome = done; });
        [&] { QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 10000); }();
        return outcome.value_or(std::unexpected(u"nothing came"_s));
    }
};

// whisper.cpp's command, standing in: it writes the words it was "told" as
// whisper-cli -oj does, to the file -of names, and says what it was given.
[[nodiscard]] QString standInWhisper(const QString &folder) {
    return Fixtures::script(folder + "/whisper-cli", R"(
echo "$@" > "$(dirname "$0")/whisper-args"
while [ $# -gt 0 ]; do [ "$1" = "-of" ] && out="$2"; shift; done
cat > "$out.json" <<'JSON'
{"result": {"language": "en"}, "transcription": [
 {"offsets": {"from": 0, "to": 400}, "text": " Good"},
 {"offsets": {"from": 400, "to": 900}, "text": " morning."},
 {"offsets": {"from": 1200, "to": 1600}, "text": " Again"}]}
JSON
)");
}

// A server that fails the test the moment anything is asked of it.
struct Untouchable : FakeServer {
    Untouchable() {
        answering = [](const Request &request) {
            QTest::qFail(qPrintable(u"Asked with the AI off: "_s + QString::fromLatin1(request.line)), __FILE__, __LINE__);
            return Reply{.status = "500 Internal Server Error", .body = "{}"};
        };
    }
};

// A plugin of these functions on the desk's settings and library, loaded.
[[nodiscard]] std::unique_ptr<ScriptPlugin> pluginOf(Desk &desk, const QString &script) {
    const QString folder = desk.dir.filePath("plugins/notebook");
    QDir().mkpath(folder + "/data");
    Fixtures::write(folder + "/plugin.json", R"({"name": "Notebook", "version": "1.0", "script": "main.js"})");
    Fixtures::write(folder + "/main.js", script.toUtf8());
    ScriptServices services;
    services.settings = &desk.settings;
    services.library = [&desk] { return &desk.library; };
    auto plugin = std::make_unique<ScriptPlugin>(ScriptManifest::read(folder), services);
    QString problem;
    [&] { QVERIFY2(plugin->load(&problem), qPrintable(problem)); }();
    return plugin;
}

constexpr auto statusScript = R"js(
function status(kind, hosted) { return omabook.recognize.status(kind, hosted ? {hosted: true} : undefined); }
function available(kind, hosted) { return omabook.recognize.available(kind, {hosted: hosted}); }
function read(items, hosted) { return omabook.recognize.ink(items, {hosted: hosted}); }
)js";

} // namespace

class RecognitionTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;

private slots:
    void initTestCase() { Secrets::useInstead(&m_secrets); }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    void inkIsDrawnAsAPicture() {
        const QImage picture = Ink::picture(writing());
        QVERIFY(!picture.isNull());
        QVERIFY(picture.width() <= Ink::pictureSide && picture.height() <= Ink::pictureSide);
        // White paper, the writing black whatever its colour.
        QCOMPARE(picture.pixelColor(0, 0), QColor(Qt::white));
        bool inked = false;
        for (int y = 0; y < picture.height() && !inked; ++y)
            for (int x = 0; x < picture.width() && !inked; ++x)
                inked = qGray(picture.pixel(x, y)) < 60;
        QVERIFY(inked);
        QVERIFY(QImage::fromData(Ink::png(writing())).size() == picture.size());
        // Nothing to draw is no picture; a single dot is one.
        QVERIFY(Ink::picture({}).isNull());
        std::vector<Ink::Stroke> dot{{.id = u"d"_s,
                                      .color = Qt::black,
                                      .width = 4,
                                      .pressured = false,
                                      .tilted = false,
                                      .began = 1,
                                      .points = {{.at = {5, 5}, .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = 0}}}};
        QVERIFY(!Ink::picture(dot).isNull());
    }

    void aKeyIsTheInputAndTheModel() {
        const auto first = Recognition::key(ink(), u"llava"_s);
        QVERIFY2(first, qPrintable(whyNot(first)));
        // The same writing, kept in another order and under other ids, is the same.
        std::vector<Ink::Stroke> reordered = writing();
        std::ranges::reverse(reordered);
        for (Ink::Stroke &stroke : reordered)
            stroke.id = Ink::newId();
        QCOMPARE(Recognition::key(ink(reordered), u"llava"_s).value_or(QString()), *first);
        // Another model, other ink, another language hint: another key.
        QVERIFY(Recognition::key(ink(), u"qwen2.5vl"_s).value_or(QString()) != *first);
        QVERIFY(Recognition::key(ink(writing(1)), u"llava"_s).value_or(QString()) != *first);
        Recognition::Input dutch = ink();
        dutch.language = u"nl"_s;
        QVERIFY(Recognition::key(dutch, u"llava"_s).value_or(QString()) != *first);
        QVERIFY(!Recognition::key(ink({}), u"llava"_s));

        // A recording by its bytes.
        QTemporaryDir dir;
        Fixtures::write(dir.filePath("a.wav"), "RIFF one");
        Fixtures::write(dir.filePath("b.wav"), "RIFF one");
        const Recognition::Input a{.kind = Recognition::Kind::Audio, .strokes = {}, .file = dir.filePath("a.wav"), .language = {}};
        const Recognition::Input b{.kind = Recognition::Kind::Audio, .strokes = {}, .file = dir.filePath("b.wav"), .language = {}};
        QCOMPARE(Recognition::key(a, u"whisper-1"_s).value_or(u"a"_s), Recognition::key(b, u"whisper-1"_s).value_or(u"b"_s));
        const auto missing =
            Recognition::key({.kind = Recognition::Kind::Audio, .strokes = {}, .file = dir.filePath("none.wav"), .language = {}}, {});
        QVERIFY(!missing && missing.error().contains("can't be read"));
    }

    void onlyOwnServersAreSentAnythingByDefault() {
        using Recognition::Reach;
        QCOMPARE(Recognition::reach(u"http://localhost:11434/v1"_s), Reach::ThisComputer);
        QCOMPARE(Recognition::reach(u"http://127.0.0.1:8080/v1"_s), Reach::ThisComputer);
        QCOMPARE(Recognition::reach(u"http://100.101.102.103:11434/v1"_s), Reach::Tailnet);
        QCOMPARE(Recognition::reach(u"https://desk.tail1234.ts.net/v1"_s), Reach::Tailnet);
        QCOMPARE(Recognition::reach(u"http://desk:11434/v1"_s), Reach::Tailnet);
        QCOMPARE(Recognition::reach(u"https://api.openai.com/v1"_s), Reach::Hosted);
        QCOMPARE(Recognition::reach(u"http://192.168.1.5:8000/v1"_s), Reach::Hosted);
        QCOMPARE(Recognition::reach(u"http://192.168.1.5:8000/v1"_s, {u"192.168.1.5:8000"_s}), Reach::Own);
        QCOMPARE(Recognition::reach(u"http://nas.home.arpa:8000/v1"_s, {u"http://nas.home.arpa"_s}), Reach::Own);

        Desk desk;
        const QString hosted = u"https://api.example.com/v1"_s;
        QVERIFY(RecognitionRoute::whyNot(desk.settings, u"http://localhost:11434/v1"_s, u"llava"_s, false).isEmpty());
        // A hosted server: not unless allowed in the settings and asked for.
        const QString refused = RecognitionRoute::whyNot(desk.settings, hosted, u"llava"_s, true);
        QVERIFY2(refused.contains("api.example.com is someone else's computer") && refused.contains("in the settings"),
                 qPrintable(refused));
        desk.settings.setRecognitionHosted(true);
        QVERIFY(RecognitionRoute::whyNot(desk.settings, hosted, u"llava"_s, false).contains("only where you chose it"));
        QVERIFY(RecognitionRoute::whyNot(desk.settings, hosted, u"llava"_s, true).isEmpty());
        // A model that runs elsewhere is someone else's computer, however near its server.
        desk.settings.setRecognitionHosted(false);
        QVERIFY(
            RecognitionRoute::whyNot(desk.settings, u"http://localhost:11434/v1"_s, u"qwen3-vl:cloud"_s, true).contains("qwen3-vl:cloud"));
        // And an address of one's own is one's own.
        desk.choose({{"own", QJsonArray{u"api.example.com"_s}}});
        QVERIFY(RecognitionRoute::whyNot(desk.settings, hosted, u"llava"_s, false).isEmpty());

        // The rule holds where the ways are chosen: nothing is asked of a hosted server.
        FakeServer never;
        desk.choose({{"ink_model", "llava"}});
        desk.serve(u"https://api.example.com/v1"_s);
        const Outcome outcome = desk.recognise(ink(), false, true);
        QVERIFY(!outcome && outcome.error().contains("someone else's computer"));
        QVERIFY(never.received.isEmpty());
    }

    void handwritingIsReadByAVisionModelAndKept() {
        Desk desk;
        FakeServer server;
        desk.serve(server.url());
        desk.choose({{"ink_model", "llava"}});
        server.read(u"<think>Looks like two words.</think>Hello\nworld"_s);
        const Outcome read = desk.recognise(ink());
        QVERIFY2(read, qPrintable(whyNot(read)));
        QCOMPARE(read->text, u"Hello\nworld"_s);
        QCOMPARE(read->model, u"llava"_s);
        QVERIFY(!read->kept);
        QVERIFY2(read->server.contains(u"127.0.0.1:%1"_s.arg(server.serverPort())), qPrintable(read->server));

        // The model was shown the ink as a picture, and asked for its text alone.
        QCOMPARE(server.received.size(), 1);
        QVERIFY(server.received.first().line.startsWith("POST /v1/chat/completions"));
        const QJsonObject asked = server.received.first().json();
        QCOMPARE(asked.value("model").toString(), u"llava"_s);
        const QJsonArray content = asked.value("messages").toArray().at(0).toObject().value("content").toArray();
        QCOMPARE(content.at(0).toObject().value("type").toString(), u"text"_s);
        const QString image = content.at(1).toObject().value("image_url").toObject().value("url").toString();
        QVERIFY(image.startsWith("data:image/png;base64,"));
        QVERIFY(!QImage::fromData(QByteArray::fromBase64(image.mid(22).toLatin1())).isNull());

        // The same ink again is answered from the change log, without asking.
        const Outcome again = desk.recognise(ink());
        QVERIFY2(again, qPrintable(whyNot(again)));
        QVERIFY(again->kept);
        QCOMPARE(again->text, u"Hello\nworld"_s);
        QCOMPARE(server.received.size(), 1);
        // It is the plugin's data, which goes between one's computers.
        const auto key = Recognition::key(ink(), u"llava"_s);
        QVERIFY(RecognitionStore::find(desk.library, u"notebook"_s, key.value_or(QString())).has_value());
        QVERIFY(!RecognitionStore::find(desk.library, u"another"_s, key.value_or(QString())).has_value());

        // Changed ink, or asked anew, is read again.
        server.read(u"Hello world!"_s);
        QCOMPARE(desk.recognise(ink(writing(5))).value_or(Recognition::Result{}).text, u"Hello world!"_s);
        server.read(u"Hullo\nworld"_s);
        QCOMPARE(desk.recognise(ink(), true).value_or(Recognition::Result{}).text, u"Hullo\nworld"_s);
        QCOMPARE(desk.recognise(ink()).value_or(Recognition::Result{}).text, u"Hullo\nworld"_s);
        QCOMPARE(server.received.size(), 3);
    }

    void withNoModelNamedTheServerIsAsked() {
        Desk desk;
        FakeServer server;
        desk.serve(server.url());
        server.answer({{"data", QJsonArray{QJsonObject{{"id", "qwen3-vl:cloud"}}, QJsonObject{{"id", "gemma3"}}}}});
        server.read(u"a note"_s);
        const Outcome read = desk.recognise(ink());
        QVERIFY2(read, qPrintable(whyNot(read)));
        QVERIFY(server.received.first().line.startsWith("GET /v1/models"));
        // Never the one that sends the picture on elsewhere.
        QCOMPARE(server.received.at(1).json().value("model").toString(), u"gemma3"_s);
    }

    void failuresAreSaidInPlainWords() {
        Desk desk;
        FakeServer server;
        desk.serve(server.url());
        desk.choose({{"ink_model", "llama3"}});
        server.answer({{"error", QJsonObject{{"message", "this model does not support images"}}}}, "400 Bad Request");
        QVERIFY(desk.recognise(ink()).error().contains("The model llama3 on the AI server at 127.0.0.1"));
        QVERIFY(desk.recognise(ink()).error().contains("answered with an error (500)"));
        server.answer({{"error", QJsonObject{{"message", "model \"llama3\" not found"}}}}, "404 Not Found");
        QVERIFY(desk.recognise(ink()).error().contains("has no model \"llama3\""));
        server.answer({{"error", "unauthorized"}}, "401 Unauthorized");
        QVERIFY(desk.recognise(ink()).error().contains("didn't accept the key"));
        QVERIFY(desk.recognise(ink({})).error().contains("There is no ink to recognise."));
    }

    void speechIsTranscribedWordByWord() {
        Desk desk;
        FakeServer server;
        desk.serve(server.url());
        Fixtures::write(desk.dir.filePath("talk.opus"), "OggS a recording");
        server.answer(verbose());
        const Outcome heard =
            desk.recognise({.kind = Recognition::Kind::Audio, .strokes = {}, .file = desk.dir.filePath("talk.opus"), .language = u"en"_s});
        QVERIFY2(heard, qPrintable(whyNot(heard)));
        QCOMPARE(heard->text, u"Hello there. How are you?"_s);
        QCOMPARE(heard->segments.size(), 2);
        QCOMPARE(heard->segments.at(1).start, 1500);
        QCOMPARE(heard->segments.at(0).words.size(), 2);
        QCOMPARE(heard->segments.at(1).words.size(), 3);
        QCOMPARE(heard->segments.at(1).words.at(2).text, u"you?"_s);
        QCOMPARE(heard->segments.at(1).words.at(2).end, 2900);
        QCOMPARE(heard->model, u"whisper-1"_s);

        // Sent as a form, the recording as a file, asked for each word's moment.
        const FakeServer::Request &sent = server.received.first();
        QVERIFY(sent.line.startsWith("POST /v1/audio/transcriptions"));
        QVERIFY(sent.body.contains("filename=\"talk.opus\""));
        QVERIFY(sent.body.contains("OggS a recording"));
        QVERIFY(sent.body.contains("verbose_json"));
        QVERIFY(sent.body.contains("name=\"timestamp_granularities[]\""));
        QVERIFY(sent.body.contains("name=\"language\""));

        // A server of its own for speech, with a model of its own.
        FakeServer whisper;
        desk.choose({{"transcription_url", whisper.url() + u'/'}, {"transcription_model", "Systran/faster-whisper-small"}});
        whisper.answer({{"text", "Just text."}});
        Fixtures::write(desk.dir.filePath("other.wav"), "RIFF another");
        const Outcome plain =
            desk.recognise({.kind = Recognition::Kind::Audio, .strokes = {}, .file = desk.dir.filePath("other.wav"), .language = {}});
        QVERIFY2(plain, qPrintable(whyNot(plain)));
        QCOMPARE(plain->text, u"Just text."_s);
        QVERIFY(whisper.received.first().body.contains("Systran/faster-whisper-small"));
    }

    void whisperCppTranscribesWhereNoServerDoes() {
        Desk desk;
        FakeServer server;
        desk.serve(server.url());
        const QString program = standInWhisper(desk.dir.path());
        Fixtures::write(desk.dir.filePath("ggml-base.bin"), "a model");
        desk.choose({{"whisper_program", program}, {"whisper_model", desk.dir.filePath("ggml-base.bin")}});
        Fixtures::write(desk.dir.filePath("talk.wav"), "RIFF talk");
        // An AI server that doesn't transcribe (Ollama doesn't).
        server.answer({}, "404 Not Found");
        const Outcome heard =
            desk.recognise({.kind = Recognition::Kind::Audio, .strokes = {}, .file = desk.dir.filePath("talk.wav"), .language = {}});
        QVERIFY2(heard, qPrintable(whyNot(heard)));
        QCOMPARE(heard->server, u"whisper.cpp on this computer"_s);
        QCOMPARE(heard->model, u"ggml-base"_s);
        QCOMPARE(heard->text, u"Good morning. Again"_s);
        QCOMPARE(heard->segments.size(), 2);
        QCOMPARE(heard->segments.at(0).words.at(1).end, 900);
        QCOMPARE(heard->segments.at(1).start, 1200);
        QFile args(desk.dir.filePath("whisper-args"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QByteArray given = args.readAll();
        QVERIFY(given.contains("-ml 1 -sow") && given.contains("-l auto") && given.contains(desk.dir.filePath("talk.wav").toUtf8()));

        // Without whisper.cpp either, both are said.
        desk.choose({{"whisper_program", desk.dir.filePath("no-such-whisper")}});
        server.answer({}, "404 Not Found");
        const Outcome nothing =
            desk.recognise({.kind = Recognition::Kind::Audio, .strokes = {}, .file = desk.dir.filePath("talk.wav"), .language = {}});
        QVERIFY(!nothing);
        QVERIFY2(nothing.error().contains("doesn't transcribe speech") && nothing.error().contains("whisper.cpp isn't installed"),
                 qPrintable(whyNot(nothing)));

        // A hosted server not chosen leaves whisper.cpp alone to do it.
        desk.serve(u"https://api.example.com/v1"_s);
        desk.choose({{"whisper_program", program}, {"whisper_model", desk.dir.filePath("ggml-base.bin")}});
        const Outcome local =
            desk.recognise({.kind = Recognition::Kind::Audio, .strokes = {}, .file = desk.dir.filePath("talk.wav"), .language = {}}, true);
        QVERIFY2(local && local->server == u"whisper.cpp on this computer"_s, qPrintable(whyNot(local)));
    }

    void theQueueWaitsForAServerThatIsAway() {
        Desk desk;
        const quint16 port = freePort();
        desk.serve(u"http://127.0.0.1:%1/v1"_s.arg(port));
        desk.choose({{"ink_model", "llava"}});
        desk.queue->setRetry(50ms, 200ms);
        QStringList waited;
        std::optional<Outcome> outcome;
        desk.queue->submit(
            {.input = ink(), .owner = u"notebook"_s, .hosted = false, .again = false}, [&outcome](const Outcome &done) { outcome = done; },
            [&waited](const QString &why) { waited << why; });
        QTRY_VERIFY(waited.size() >= 2);
        QVERIFY2(waited.first().contains("can't be reached now"), qPrintable(waited.first()));
        QVERIFY(!outcome);
        // The server comes back: what waited is read.
        FakeServer back(port);
        QVERIFY(back.isListening());
        back.read(u"Back again"_s);
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 5000);
        QCOMPARE(outcome->value_or(Recognition::Result{}).text, u"Back again"_s);
    }

    void aRecognitionCanBeCancelled() {
        Desk desk;
        desk.serve(u"http://127.0.0.1:%1/v1"_s.arg(freePort()));
        desk.choose({{"ink_model", "llava"}});
        desk.queue->setRetry(50ms, 50ms);
        QStringList outcomes;
        const auto told = [&outcomes](const Outcome &done) { outcomes << (done ? done->text : done.error()); };
        bool waiting = false;
        const int first = desk.queue->submit({.input = ink(), .owner = u"notebook"_s, .hosted = false, .again = false}, told,
                                             [&waiting](const QString &) { waiting = true; });
        const int second = desk.queue->submit({.input = ink(writing(2)), .owner = u"notebook"_s, .hosted = false, .again = false}, told);
        QCOMPARE(desk.queue->pending(), 2);
        QTRY_VERIFY(waiting);
        // The one waiting its turn, then the one under way.
        QVERIFY(desk.queue->cancel(second));
        QVERIFY(desk.queue->cancel(first));
        QVERIFY(!desk.queue->cancel(first));
        QCOMPARE(outcomes, (QStringList{u"The recognition was cancelled."_s, u"The recognition was cancelled."_s}));
        QCOMPARE(desk.queue->pending(), 0);
    }

    void hoursOfSpeechAreKeptWhole() {
        Desk desk;
        Recognition::Result long_{.kind = Recognition::Kind::Audio,
                                  .text = {},
                                  .segments = {},
                                  .language = u"en"_s,
                                  .model = u"whisper-1"_s,
                                  .server = u"here"_s,
                                  .at = 1234,
                                  .kept = false};
        // Two hours: some twenty thousand words, and a pause-less stretch of five thousand.
        for (int s = 0; s < 1500; ++s) {
            Recognition::Segment segment{.text = {}, .start = s * 4000, .end = s * 4000 + 3500, .words = {}};
            for (int w = 0; w < 12; ++w)
                segment.words.append({.text = u"word%1"_s.arg(w), .start = segment.start + w * 290, .end = segment.start + w * 290 + 250});
            segment.text = u"Segment %1."_s.arg(s);
            long_.segments.append(segment);
        }
        Recognition::Segment endless{.text = u"On and on"_s, .start = 6'000'000, .end = 7'000'000, .words = {}};
        for (int w = 0; w < 5000; ++w)
            endless.words.append({.text = u"on%1"_s.arg(w), .start = 6'000'000 + w * 200, .end = 6'000'000 + w * 200 + 150});
        long_.segments.append(endless);
        const auto kept = RecognitionStore::keep(desk.library, u"notebook"_s, u"k"_s, long_);
        QVERIFY2(kept, qPrintable(whyNot(kept)));
        const auto found = RecognitionStore::find(desk.library, u"notebook"_s, u"k"_s);
        QVERIFY(found.has_value());
        qsizetype words = 0;
        for (const Recognition::Segment &segment : found->segments)
            words += segment.words.size();
        QCOMPARE(words, qsizetype{1500 * 12 + 5000});
        QCOMPARE(found->segments.first().words.at(3).start, 3 * 290);
        QCOMPARE(found->segments.last().words.last().text, u"on4999"_s);
        QVERIFY(found->kept);

        // Kept anew, the earlier parts go.
        Recognition::Result short_ = long_;
        short_.segments = {{.text = u"Short."_s, .start = 0, .end = 10, .words = {}}};
        short_.at = 5678;
        QVERIFY(RecognitionStore::keep(desk.library, u"notebook"_s, u"k"_s, short_));
        const QJsonValue thing = PluginData::read(desk.library, {.plugin = u"notebook"_s, .entity = u"omabook-recognized"_s, .id = u"k"_s});
        QCOMPARE(thing.toObject().value("sets").toObject().value("parts").toObject().size(), 1);
        QCOMPARE(RecognitionStore::find(desk.library, u"notebook"_s, u"k"_s).value_or(Recognition::Result{}).text, u"Short."_s);
    }

    // With the AI set to Off (#133) nothing is asked of any server, the
    // person's own included: not to read handwriting, not to transcribe, not
    // to embed a book's text, not to answer; and a plugin is told so.
    void withTheAiOffNothingIsAskedOfAnyServer() {
        Desk desk;
        Untouchable server;
        desk.serve(server.url());
        const QString whisper = standInWhisper(desk.dir.path());
        Fixtures::write(desk.dir.filePath("ggml-base.bin"), "model");
        desk.choose({{"ink_model", "llava"},
                     {"transcription_url", server.url()},
                     {"whisper_program", whisper},
                     {"whisper_model", desk.dir.filePath("ggml-base.bin")}});
        desk.settings.setMeaning({{"url", server.url()}});
        desk.settings.setAiProvider(u"off"_s);
        QVERIFY(desk.settings.aiOff());

        for (const auto kind : {Recognition::Kind::Ink, Recognition::Kind::Audio}) {
            const auto refused = RecognitionRoute::refusal(desk.settings, kind, true);
            QVERIFY(refused && refused->lack == RecognitionRoute::Lack::AiOff);
            QVERIFY(!RecognitionRoute::ways(desk.settings, kind, true));
        }
        const Outcome read = desk.recognise(ink(), false, true);
        QVERIFY2(!read && read.error().contains("turned off"), qPrintable(whyNot(read)));
        Fixtures::write(desk.dir.filePath("talk.wav"), "RIFF talk");
        const Outcome heard =
            desk.recognise({.kind = Recognition::Kind::Audio, .strokes = {}, .file = desk.dir.filePath("talk.wav"), .language = {}});
        QVERIFY(!heard && heard.error().contains("turned off"));
        // Not even whisper.cpp on this computer is run.
        QVERIFY(!QFile::exists(desk.dir.filePath("whisper-args")));
        const auto route = Embeddings::route(desk.settings);
        QVERIFY(!route && route.error().code() == Failure::Code::AiOff);
        QVERIFY(makeAiProvider(desk.settings) == nullptr);

        const auto plugin = pluginOf(desk, QString::fromLatin1(statusScript));
        QString problem;
        QCOMPARE(plugin->callNow(u"available"_s, {u"ink"_s, true}, &problem), QJsonValue(false));
        QCOMPARE(plugin->callNow(u"available"_s, {u"audio"_s, false}, &problem), QJsonValue(false));
        const QJsonObject status = plugin->callNow(u"status"_s, {u"ink"_s, true}, &problem).toObject();
        QCOMPARE(status.value("available"), QJsonValue(false));
        QCOMPARE(status.value("why").toString(), u"ai-off"_s);
        QVERIFY(status.value("message").toString().contains("turned off"));
        std::optional<QString> rejected;
        plugin->callFunction(u"read"_s, {Ink::items(writing()), true},
                             [&rejected](const QJsonValue &, const QString &why) { rejected = why; });
        QTRY_VERIFY(rejected.has_value());
        QVERIFY2(rejected->contains("turned off"), qPrintable(*rejected));
        // Anything still on its way would have arrived by now.
        QTest::qWait(200);
        QVERIFY(server.received.isEmpty());
    }

    // recognize.status says why a plugin can't have recognition, by a code
    // (#133); recognize.available stays the boolean it was, and takes {hosted}.
    void aPluginIsToldWhyItCantRecognise() {
        Desk desk;
        FakeServer server;
        desk.choose({{"ink_model", "llava"}, {"whisper_program", desk.dir.filePath("none/whisper-cli")}});
        desk.settings.setAiEndpoint({{"preset", "custom"}, {"base_url", ""}});
        const auto plugin = pluginOf(desk, QString::fromLatin1(statusScript));
        QString problem;
        const auto why = [&plugin, &problem](const QString &kind, bool hosted = false) {
            return plugin->callNow(u"status"_s, {kind, hosted}, &problem).toObject().value("why").toString();
        };
        QCOMPARE(why(u"ink"_s), u"no-server"_s);
        QCOMPARE(why(u"audio"_s), u"no-server"_s);
        QCOMPARE(why(u"paper"_s), u"unknown-kind"_s);
        desk.serve(u"https://api.example.com/v1"_s);
        QCOMPARE(why(u"ink"_s, true), u"hosted-not-allowed"_s);
        desk.settings.setRecognitionHosted(true);
        QCOMPARE(why(u"ink"_s), u"hosted-not-chosen"_s);
        QCOMPARE(why(u"ink"_s, true), QString());
        QCOMPARE(plugin->callNow(u"available"_s, {u"ink"_s, true}, &problem), QJsonValue(true));
        QCOMPARE(plugin->callNow(u"available"_s, {u"ink"_s, false}, &problem), QJsonValue(false));

        // A server without a model that reads pictures is learnt of by asking it, until the model is another.
        desk.serve(server.url());
        QCOMPARE(why(u"ink"_s), QString());
        server.answer({{"error", QJsonObject{{"message", "llava does not support images"}}}}, "400 Bad Request");
        std::optional<QString> refused;
        plugin->callFunction(u"read"_s, {Ink::items(writing()), false},
                             [&refused](const QJsonValue &, const QString &said) { refused = said; });
        QTRY_VERIFY(refused.has_value());
        QVERIFY2(refused->contains("can't read pictures"), qPrintable(*refused));
        const QJsonObject lacking = plugin->callNow(u"status"_s, {u"ink"_s, false}, &problem).toObject();
        QCOMPARE(lacking.value("why").toString(), u"no-model"_s);
        QCOMPARE(lacking.value("message").toString(), *refused);
        QCOMPARE(plugin->callNow(u"available"_s, {u"ink"_s, false}, &problem), QJsonValue(false));
        desk.choose({{"ink_model", "qwen3-vl"}});
        QCOMPARE(why(u"ink"_s), QString());
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    void aPluginRecognisesInkAndSpeech() {
        Desk desk;
        FakeServer server;
        desk.serve(server.url());
        desk.choose({{"ink_model", "llava"}});
        const QString folder = desk.dir.filePath("plugins/notebook");
        QDir().mkpath(folder + "/data");
        Fixtures::write(folder + "/plugin.json", R"({"name": "Notebook", "version": "1.0", "script": "main.js"})");
        Fixtures::write(folder + "/main.js", R"js(
function read(items) { return omabook.recognize.ink(items, {language: "en"}); }
function hear(file) { return omabook.recognize.audio(file); }
function hosted(items) { return omabook.recognize.ink(items, {hosted: true}); }
function stop(items) { var asked = omabook.recognize.ink(items); return typeof asked.cancel === "function" && asked.cancel(); }
function can() { return [omabook.recognize.available("ink"), omabook.recognize.available("paper")]; }
function wrong() { try { omabook.recognize.ink("strokes"); return ""; } catch (error) { return String(error.message); } }
)js");
        Fixtures::write(folder + "/data/talk.wav", "RIFF talk");
        ScriptServices services;
        services.settings = &desk.settings;
        services.library = [&desk] { return &desk.library; };
        ScriptPlugin plugin(ScriptManifest::read(folder), services);
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));

        const auto call = [&plugin](const QString &function, const QJsonArray &arguments) {
            std::optional<std::pair<QJsonValue, QString>> answer;
            plugin.callFunction(function, arguments, [&answer](const QJsonValue &value, const QString &why) { answer = {{value, why}}; });
            [&] { QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 10000); }();
            return answer.value_or(std::pair<QJsonValue, QString>{});
        };
        const std::vector<Ink::Stroke> strokes = writing();
        const QJsonObject items = Ink::items(strokes);

        server.read(u"From the notebook"_s);
        const auto [read, readProblem] = call(u"read"_s, {items});
        QVERIFY2(readProblem.isEmpty(), qPrintable(readProblem));
        QCOMPARE(read.toObject().value("text").toString(), u"From the notebook"_s);
        QCOMPARE(read.toObject().value("kind").toString(), u"ink"_s);
        // Kept with the plugin's own data, by the hash of the ink and the model.
        Recognition::Input given = ink(strokes);
        given.language = u"en"_s;
        QVERIFY(RecognitionStore::find(desk.library, u"notebook"_s, Recognition::key(given, u"llava"_s).value_or(QString())));

        server.answer(verbose());
        const auto [heard, hearProblem] = call(u"hear"_s, {u"data/talk.wav"_s});
        QVERIFY2(hearProblem.isEmpty(), qPrintable(hearProblem));
        QCOMPARE(heard.toObject().value("segments").toArray().at(1).toObject().value("words").toArray().size(), 3);
        // Only a file of its own folder.
        const auto [outside, outsideProblem] = call(u"hear"_s, {u"../../config.json"_s});
        QVERIFY2(outsideProblem.contains("plugin's own folder"), qPrintable(outsideProblem));

        // A plugin can't send ink to a hosted server the person didn't allow.
        desk.serve(u"https://api.example.com/v1"_s);
        const auto [refused, why] = call(u"hosted"_s, {items});
        QVERIFY2(why.contains("someone else's computer"), qPrintable(why));
        QCOMPARE(plugin.callNow(u"can"_s, {}, &problem).toArray(), (QJsonArray{false, false}));
        desk.serve(server.url());
        QCOMPARE(plugin.callNow(u"can"_s, {}, &problem).toArray(), (QJsonArray{true, false}));
        QVERIFY(plugin.callNow(u"stop"_s, {Ink::items(writing(3))}, &problem).toBool());
        QVERIFY(plugin.callNow(u"wrong"_s, {}, &problem).toString().contains("The strokes should be"));
    }
};

QTEST_MAIN(RecognitionTests)
#include "recognition_tests.moc"
