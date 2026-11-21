// The registry of AI capabilities (#143, ADR 0008): today's settings carried
// over unchanged from an older config.json, a new one starting local, a
// hosted choice per capability, status explaining each with a code, AI Off,
// and all six offered to a plugin through `omabook.ai`.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <optional>

#include "ai.h"
#include "aiprivacy.h"
#include "airegistry.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "library.h"
#include "scriptplugin.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QJsonObject fileOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

[[nodiscard]] QString why(const Settings &settings, Capability capability, const AiRegistry::Ask &ask = {}) {
    return Capabilities::lackName(AiRegistry::status(settings, capability, ask).why);
}

// What a provider answered, or why it didn't, for one request.
[[nodiscard]] std::optional<Failure> failureOf(AiProvider &provider, AiRequest request) {
    QSignalSpy finished(&provider, &AiProvider::finished);
    QSignalSpy failed(&provider, &AiProvider::failed);
    provider.ask(request);
    if (!QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 5000))
        return Failure::said(u"no answer"_s);
    return failed.isEmpty() ? std::nullopt : std::optional<Failure>(failed.first().first().value<Failure>());
}

[[nodiscard]] AiRequest asking(AiRequest::Tier tier) {
    AiRequest request;
    request.tier = tier;
    request.messages.append({u"user"_s, u"Who is Ishmael?"_s});
    return request;
}

// Answers POST /embeddings with a vector for each input.
FakeServer::Reply embedded(const FakeServer::Request &request) {
    QJsonArray data;
    const QJsonArray input = request.json().value("input").toArray();
    for (qsizetype at = 0; at < input.size(); ++at)
        data.append(QJsonObject{{"index", at}, {"embedding", QJsonArray{3.0, 4.0}}});
    return {.status = "200 OK", .body = QJsonDocument(QJsonObject{{"data", data}}).toJson(QJsonDocument::Compact)};
}

constexpr const char *everything = R"(
function status(options) { return omabook.ai.status(options); }
function ask(options) { return omabook.ai.ask("Summarise my notes.", options); }
function heard(hosted) { return omabook.recognize.status("audio", {hosted: hosted}); }
function speak(options) { return omabook.ai.speak("Call me Ishmael.", options); }
function embed(options) { return omabook.ai.embed(["Call me Ishmael.", "Some years ago"], options); }
)";

struct Bench {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    Library library{dir.filePath("Library")};

    // Omabook's first start here: a config.json written new.
    Bench() {
        library.open();
        AiPrivacy::carryOver(settings);
    }

    std::unique_ptr<ScriptPlugin> plugin() {
        const QString folder = dir.filePath("plugins/notes");
        QDir().mkpath(folder + "/data");
        const QJsonObject manifest{{"name", "Notes"}, {"version", "1.0"}, {"script", "main.js"}};
        Fixtures::write(folder + "/plugin.json", QJsonDocument(manifest).toJson());
        Fixtures::write(folder + "/main.js", everything);
        ScriptServices services;
        services.settings = &settings;
        services.library = [this] { return &library; };
        services.ai = AiRegistry::answerer(settings);
        auto made = std::make_unique<ScriptPlugin>(ScriptManifest::read(folder), services);
        QString problem;
        [&] { QVERIFY2(made->load(&problem), qPrintable(problem)); }();
        return made;
    }
};

struct Answer {
    QJsonValue result;
    QString problem;
};
Answer called(ScriptPlugin &plugin, const QString &function, const QJsonArray &arguments = {}) {
    std::optional<Answer> answer;
    plugin.callFunction(function, arguments,
                        [&answer](const QJsonValue &result, const QString &problem) { answer = Answer{result, problem}; });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 10000))
        return {{}, u"no answer"_s};
    return *answer;
}

} // namespace

class AiRegistryTests : public QObject {
    Q_OBJECT

private slots:
    // An older config.json keeps every choice it made: answers and quick their
    // provider, with hosted allowed where it was hosted, and the others the
    // "hosted" of their own keys, which are left as they were.
    void anOlderConfigKeepsItsChoices() {
        QTemporaryDir dir;
        const QString path = dir.filePath("config.json");
        const QJsonObject older{
            {"config_version", 3},
            {"ai_provider", "command"},
            {"ai_command", "cat"},
            {"ai_models", QJsonObject{{"command", QJsonObject{{"main", "big"}, {"quick", "small"}}}}},
            {"recognition", QJsonObject{{"hosted", true}, {"ink_model", "qwen3-vl"}, {"own", QJsonArray{"nas.example.com"}}}},
            {"meaning", QJsonObject{{"url", "https://embeddings.example.com/v1"}, {"hosted", false}, {"all", true}}},
            {"read_aloud", QJsonObject{{"engine", "piper"}, {"voice", "alan"}, {"rate", 1.25}, {"hosted", true}}},
        };
        QVERIFY(Fixtures::write(path, QJsonDocument(older).toJson()));

        Settings settings(path);
        QVERIFY(!settings.hasCapabilities());
        // Read the old way until it is written: what is chosen never depends on it having been.
        QVERIFY(AiPrivacy::hostedAllowed(settings, Capability::Answers));
        AiPrivacy::carryOver(settings);
        QVERIFY(settings.hasCapabilities());
        const QJsonObject capabilities = fileOf(path).value("capabilities").toObject();
        QCOMPARE(capabilities.value("answers").toObject(), (QJsonObject{{"provider", "command"}, {"hosted", true}}));
        QCOMPARE(capabilities.value("quick").toObject(), (QJsonObject{{"provider", "command"}, {"hosted", true}}));
        QCOMPARE(capabilities.value("handwriting").toObject(), (QJsonObject{{"hosted", true}}));
        QCOMPARE(capabilities.value("speech-to-text").toObject(), (QJsonObject{{"hosted", true}}));
        QCOMPARE(capabilities.value("text-to-speech").toObject(), (QJsonObject{{"hosted", true}}));
        QCOMPARE(capabilities.value("embeddings").toObject(), (QJsonObject{{"hosted", false}}));

        // The older keys stay as they were, for an older Omabook on the same file.
        const QJsonObject written = fileOf(path);
        for (auto key = older.begin(); key != older.end(); ++key)
            QCOMPARE(written.value(key.key()), key.value());
        // And each choice is used as before: the provider, its two models.
        QCOMPARE(settings.aiProvider(), QString("command"));
        const CapabilityStatus answers = AiRegistry::status(settings, Capability::Answers);
        QVERIFY2(answers.available, qPrintable(answers.failure.text()));
        QVERIFY(answers.hosted);
        QCOMPARE(answers.model, QString("big"));
        QCOMPARE(AiRegistry::status(settings, Capability::Quick).model, QString("small"));
        QCOMPARE(why(settings, Capability::Embeddings), QString("hosted-not-allowed"));

        // Carried over once: what is chosen afterwards is kept.
        QJsonObject chosen = settings.capabilities();
        chosen.insert("embeddings", QJsonObject{{"hosted", true}});
        settings.setCapabilities(chosen);
        AiPrivacy::carryOver(settings);
        QVERIFY(AiPrivacy::hostedAllowed(settings, Capability::Embeddings));
    }

    // Omarchy's default agent is carried over as itself; a server of one's own
    // gets no hosted allowed, one elsewhere does; Off is no provider and stays Off.
    void eachOlderProviderIsCarriedOverAsItWas() {
        QTemporaryDir dir;
        const auto carried = [&dir](const QJsonObject &older) {
            const QString path = dir.filePath(QString::number(qHash(QJsonDocument(older).toJson())) + ".json");
            Fixtures::write(path, QJsonDocument(older).toJson());
            Settings settings(path);
            return AiPrivacy::carriedOver(settings).value("answers").toObject();
        };
        QCOMPARE(carried({{"font_size", 20}}), (QJsonObject{{"provider", "omarchy-default"}, {"hosted", true}}));
        QCOMPARE(carried({{"ai_provider", "auto"}}), (QJsonObject{{"provider", "omarchy-default"}, {"hosted", true}}));
        QCOMPARE(carried({{"ai_provider", "anthropic"}}), (QJsonObject{{"provider", "anthropic"}, {"hosted", true}}));
        const QJsonObject ollama{{"ai_provider", "openai"}, {"ai_endpoint", QJsonObject{{"preset", "ollama"}}}};
        QCOMPARE(carried(ollama), (QJsonObject{{"provider", "openai"}, {"hosted", false}}));
        QJsonObject cloud = ollama;
        cloud.insert("ai_models", QJsonObject{{"openai:http://localhost:11434/v1", QJsonObject{{"main", "gpt-oss:120b-cloud"}}}});
        QCOMPARE(carried(cloud), (QJsonObject{{"provider", "openai"}, {"hosted", true}}));
        const QJsonObject elsewhere{{"ai_provider", "openai"},
                                    {"ai_endpoint", QJsonObject{{"preset", "custom"}, {"base_url", "https://api.example.com/v1"}}}};
        QCOMPARE(carried(elsewhere), (QJsonObject{{"provider", "openai"}, {"hosted", true}}));
        QCOMPARE(carried({{"ai_provider", "off"}}), (QJsonObject{{"hosted", false}}));
    }

    // A config.json written new starts local: answers from a server of one's
    // own, hosted allowed for nothing, and a hosted provider refused until it is.
    void aNewConfigStartsLocal() {
        Bench bench;
        Settings &settings = bench.settings;
        QVERIFY(settings.hasCapabilities());
        QCOMPARE(settings.capability(Capability::Answers).value("provider").toString(), QString("local"));
        QCOMPARE(settings.aiProvider(), QString("openai"));
        for (const Capability capability : Capabilities::all)
            QVERIFY(!AiPrivacy::hostedAllowed(settings, capability));
        const CapabilityStatus answers = AiRegistry::status(settings, Capability::Answers);
        QVERIFY2(answers.available, qPrintable(answers.failure.text()));
        QVERIFY(!answers.hosted);

        // A command is someone else's computer for all that can be known:
        // used only where hosted is allowed for answers.
        settings.setAiCommand("cat");
        settings.setAiProvider("command");
        QCOMPARE(why(settings, Capability::Answers), QString());
        QJsonObject chosen = settings.capabilities();
        chosen.insert("answers", QJsonObject{{"provider", "command"}, {"hosted", false}});
        chosen.insert("quick", QJsonObject{{"provider", "command"}, {"hosted", false}});
        settings.setCapabilities(chosen);
        QCOMPARE(why(settings, Capability::Answers), QString("hosted-not-allowed"));
        const auto refused = AiRegistry::answers(settings);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code(), Failure::Code::AiHostedNotAllowed);

        // Chosen in today's settings is chosen for answers and quick.
        settings.setAiProvider("anthropic");
        settings.setAiProvider("command");
        QCOMPARE(why(settings, Capability::Answers), QString());
        QCOMPARE(why(settings, Capability::Quick), QString());
    }

    // Allowing hosted for answers allows nothing for quick work: one provider
    // serves both, and refuses the quick request before anything is sent.
    void quickWorkHasItsOwnHostedChoice() {
        Bench bench;
        Settings &settings = bench.settings;
        settings.setAiCommand("cat");
        settings.setAiProvider("command");
        QJsonObject chosen = settings.capabilities();
        chosen.insert("quick", QJsonObject{{"provider", "command"}, {"hosted", false}});
        settings.setCapabilities(chosen);
        QCOMPARE(why(settings, Capability::Quick), QString("hosted-not-allowed"));
        auto made = AiRegistry::answers(settings);
        QVERIFY2(made, qPrintable(Fixtures::whyNot(made)));
        const auto quick = failureOf(**made, asking(AiRequest::Quick));
        QVERIFY(quick);
        QCOMPARE(quick->code(), Failure::Code::AiQuickHostedNotAllowed);
        QCOMPARE(failureOf(**made, asking(AiRequest::Main)), std::nullopt);
    }

    // Status says per capability why it isn't available, by a code.
    void statusExplainsEachCapability() {
        Bench bench;
        Settings &settings = bench.settings;
        settings.setAiEndpoint({{"preset", "custom"}, {"base_url", ""}});
        // Neither whisper.cpp nor voxtype, whatever the machine running the tests has.
        settings.setRecognition(
            {{"whisper_program", bench.dir.filePath("none/whisper-cli")}, {"voxtype_program", bench.dir.filePath("none/voxtype")}});
        settings.setReadAloud({{"engine", "server"}});
        QCOMPARE(why(settings, Capability::Answers), QString("no-server"));
        QCOMPARE(why(settings, Capability::Handwriting), QString("no-server"));
        QCOMPARE(why(settings, Capability::SpeechToText), QString("no-server"));
        QCOMPARE(why(settings, Capability::TextToSpeech), QString("no-server"));
        QCOMPARE(why(settings, Capability::Embeddings), QString("no-server"));

        settings.setAiEndpoint({{"preset", "openai"}});
        qunsetenv("OPENAI_API_KEY");
        QCOMPARE(why(settings, Capability::Answers), QString("no-key"));

        settings.setAiEndpoint({{"preset", "custom"}, {"base_url", "https://api.example.com/v1"}});
        QCOMPARE(why(settings, Capability::Handwriting), QString("hosted-not-allowed"));
        QCOMPARE(why(settings, Capability::Embeddings), QString("hosted-not-allowed"));
        QCOMPARE(why(settings, Capability::TextToSpeech), QString("hosted-not-allowed"));
        const CapabilityStatus answers = AiRegistry::status(settings, Capability::Answers);
        QCOMPARE(Capabilities::lackName(answers.why), QString("hosted-not-allowed"));
        QVERIFY(answers.hosted);
        QVERIFY(answers.toJson().value("message").toString().contains("api.example.com"));

        settings.setAiProvider("off");
        for (const Capability capability : Capabilities::all)
            QCOMPARE(why(settings, capability), QString("ai-off"));
        QCOMPARE(AiRegistry::describe(settings).value("embeddings").toObject().value("why").toString(), QString("ai-off"));
    }

    // All six through `omabook.ai`, each with the provider the person chose:
    // answers and quick take the person's hosted choice, the other four only
    // what the plugin asked for too.
    void aPluginUsesEveryCapabilityThroughTheRegistry() {
        Bench bench;
        Settings &settings = bench.settings;
        settings.setAiCommand("cat");
        settings.setAiProvider("command");
        FakeServer server;
        server.answering = embedded;
        settings.setMeaning({{"url", server.url()}});
        settings.setReadAloud({{"engine", "server"}, {"url", "https://voice.example.com/v1"}, {"hosted", true}});
        settings.setRecognition({{"transcription_url", "https://speech.example.com/v1"},
                                 {"whisper_program", bench.dir.filePath("none/whisper-cli")},
                                 {"voxtype_program", bench.dir.filePath("none/voxtype")},
                                 {"hosted", true}});
        const auto plugin = bench.plugin();
        QString problem;
        const auto statusOf = [&](const QJsonObject &options) { return plugin->callNow("status", {options}, &problem).toObject(); };

        // Answers and quick: the person chose a command, so it is asked.
        QJsonObject status = statusOf({});
        QVERIFY2(status.value("available").toBool(), qPrintable(status.value("message").toString()));
        QCOMPARE(status.value("capability").toString(), QString("answers"));
        QCOMPARE(status.value("hosted").toBool(), true);
        QCOMPARE(statusOf({{"model", "quick"}}).value("capability").toString(), QString("quick"));
        Answer asked = called(*plugin, "ask", {QJsonObject{{"capability", "quick"}}});
        QVERIFY2(asked.problem.isEmpty(), qPrintable(asked.problem));
        QVERIFY(asked.result.toString().contains("Summarise my notes."));
        QCOMPARE(statusOf({{"hosted", false}}).value("why").toString(), QString("hosted-not-chosen"));

        // Speech to text and handwriting: hosted allowed, but only when asked for.
        QCOMPARE(statusOf({{"capability", "speech-to-text"}}).value("why").toString(), QString("hosted-not-chosen"));
        QCOMPARE(plugin->callNow("heard", {false}, &problem).toObject().value("why").toString(), QString("hosted-not-chosen"));
        status = statusOf({{"capability", "speech-to-text"}, {"hosted", true}});
        QVERIFY2(status.value("available").toBool(), qPrintable(status.value("message").toString()));
        QVERIFY(status.value("provider").toString().contains("speech.example.com"));
        // Handwriting goes to the AI server, Ollama on this computer: the person's own.
        status = statusOf({{"capability", "handwriting"}});
        QVERIFY2(status.value("available").toBool(), qPrintable(status.value("message").toString()));
        QCOMPARE(status.value("hosted").toBool(), false);

        // Text to speech: a hosted voice reads only when asked for; nothing is played otherwise.
        QCOMPARE(statusOf({{"capability", "text-to-speech"}}).value("why").toString(), QString("hosted-not-chosen"));
        QVERIFY(called(*plugin, "speak").problem.contains("voice.example.com"));

        // Embeddings: from the person's own server, as vectors of length 1.
        status = statusOf({{"capability", "embeddings"}});
        QVERIFY2(status.value("available").toBool(), qPrintable(status.value("message").toString()));
        QCOMPARE(status.value("hosted").toBool(), false);
        const Answer embedded = called(*plugin, "embed");
        QVERIFY2(embedded.problem.isEmpty(), qPrintable(embedded.problem));
        QCOMPARE(embedded.result.toArray().size(), 2);
        const QJsonArray first = embedded.result.toArray().first().toArray();
        QCOMPARE(first.size(), 2);
        QVERIFY(qAbs(first.at(0).toDouble() - 0.6) < 1e-6 && qAbs(first.at(1).toDouble() - 0.8) < 1e-6);
        QCOMPARE(server.received.size(), 1);

        QCOMPARE(statusOf({{"capability", "telepathy"}}).value("why").toString(), QString("unknown-capability"));
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    // AI Off for a plugin is AI Off: every capability says so, and nothing is
    // asked of anyone, while a local voice would still read.
    void aPluginIsRefusedEverythingWithTheAiOff() {
        Bench bench;
        Settings &settings = bench.settings;
        FakeServer server;
        server.answering = embedded;
        settings.setMeaning({{"url", server.url()}});
        settings.setReadAloud({{"engine", "server"}, {"url", server.url()}});
        settings.setAiProvider("off");
        const auto plugin = bench.plugin();
        QString problem;
        for (const Capability capability : Capabilities::all) {
            const QJsonObject status =
                plugin->callNow("status", {QJsonObject{{"capability", Capabilities::name(capability)}, {"hosted", true}}}, &problem)
                    .toObject();
            QCOMPARE(status.value("why").toString(), QString("ai-off"));
        }
        QVERIFY(called(*plugin, "ask").problem.contains("turned off"));
        QVERIFY(!called(*plugin, "embed", {QJsonObject{{"hosted", true}}}).problem.isEmpty());
        QVERIFY(!called(*plugin, "speak", {QJsonObject{{"hosted", true}}}).problem.isEmpty());
        QTest::qWait(100);
        QVERIFY(server.received.isEmpty());
    }
};

QTEST_GUILESS_MAIN(AiRegistryTests)
#include "airegistry_tests.moc"
