// The AI page's rows, each its own (#170): handwriting with a server of its
// own (an older config.json reading on as it did), a line under every row
// saying what Omabook uses it for, in the five catalogs, and a Test per row
// that sends one small real request through exactly what the row uses,
// against fake servers and programs: what came back, or why not as a code
// (no route, a refused key, no such model, no answer in time, hosted not
// allowed, the AI off), a hosted test asked first. The page surveyed in
// German, under fingers and right to left, with the tests' outcomes shown.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QTcpServer>
#include <QtTest>
#include <utility>

#include "aicapability.h"
#include "aicapabilityrows.h"
#include "aiendpoint.h"
#include "aiprivacy.h"
#include "airegistry.h"
#include "capabilitytest.h"
#include "desk.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "secretstore.h"
#include "settings.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// A server of the OpenAI-compatible API that serves every capability: its
// models, answers as they stream, a picture read, a recording heard, and
// embeddings.
FakeServer::Reply serves(const FakeServer::Request &request) {
    if (request.line.startsWith("GET"))
        return {.status = "200 OK", .body = R"({"data": [{"id": "gemma3"}, {"id": "nomic-embed-text"}]})"};
    if (request.line.contains("/embeddings")) {
        QJsonArray data;
        const QJsonArray input = request.json().value("input").toArray();
        for (qsizetype at = 0; at < input.size(); ++at)
            data.append(QJsonObject{{"index", at}, {"embedding", QJsonArray{3.0, 4.0, 0.0}}});
        return {.status = "200 OK", .body = QJsonDocument(QJsonObject{{"data", data}}).toJson(QJsonDocument::Compact)};
    }
    if (request.line.contains("/audio/transcriptions"))
        return {.status = "200 OK", .body = R"({"text": "Hello, Omabook."})"};
    if (request.json().value("stream").toBool())
        return {.status = "200 OK",
                .body = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Working \"}}]}\n\n"
                        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"fine\"}}]}\n\n"
                        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n"};
    return {.status = "200 OK", .body = R"({"model": "gemma3", "choices": [{"message": {"role": "assistant", "content": "OK"}}]})"};
}

[[nodiscard]] bool program(const QString &path, const QByteArray &script) {
    if (!Fixtures::write(path, script))
        return false;
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// What each row is used for, in English, as the page says it.
const QStringList usedFor{
    "Used for the companion's answers, catch-ups and who's who, asking about your library, connections between books, Change "
    "Omabook, and what plugins write.",
    "Used for building the knowledge graph, tidying the library (series and collections), finding a book's details, finding "
    "books by a description, the companion's suggested questions, and voice commands Omabook doesn't know itself.",
    "Used for reading your handwritten notes, the handwriting on a book's pages so that search finds it, and Notebooks' pages.",
    "Used for dictating into a note or a question, voice commands, and writing out recordings.",
    "Used for reading books aloud, reading the companion's answers aloud, and plugins that speak.",
    "Used for finding passages by what they mean in Find, and plugins that search by meaning.",
};

} // namespace

class CapabilityTestTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;
    QByteArray m_path;

    static QVariant value(const Desk &desk, const QString &name, const char *property) {
        QObject *found = desk.find(name);
        return found ? found->property(property) : QVariant();
    }
    static QString text(const Desk &desk, const QString &name) { return value(desk, name, "text").toString(); }
    static bool open(Desk &desk) {
        desk.call("showAiSettings");
        return desk.waitForPopup() && value(desk, "aiSettings", "opened").toBool();
    }
    static void press(const Desk &desk, const QString &name) {
        QObject *button = desk.find(name);
        QVERIFY2(button, qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    }
    static QVariantMap outcome(Desk &desk, Capability capability) { return desk.aiAgent->capabilities()->tester()->outcome(capability); }
    // Whether a test of it has ended, either way.
    static bool ended(Desk &desk, Capability capability) {
        const QString state = outcome(desk, capability).value("state").toString();
        return state == "passed"_L1 || state == "failed"_L1;
    }
    static QStringList posted(const FakeServer &server) {
        QStringList lines;
        for (const auto &request : server.received)
            if (request.line.startsWith("POST"))
                lines << QString::fromLatin1(request.line);
        return lines;
    }

    // Every capability with a provider of its own: the fake server for
    // answers, quick work, handwriting, speech to text and the meaning
    // index, and speech-dispatcher (a script) for the voice; local for
    // every capability and hosted for none.
    static bool fakeProviders(Desk &desk, const FakeServer &server) {
        const bool made = QDir().mkpath(desk.dir.filePath("bin")) && program(desk.dir.filePath("bin/spd-say"), "#!/bin/bash\nexit 0\n");
        qputenv("PATH", desk.dir.filePath("bin").toUtf8());
        desk.settings->setCapabilities(Capabilities::fresh());
        desk.settings->setAiEndpoint({{"preset", "custom"}, {"base_url", server.url()}});
        desk.settings->chooseAiProvider("openai");
        desk.settings->setRecognition({{"ink_model", "gemma3"}});
        QJsonObject all = desk.settings->capabilities();
        all.insert("speech-to-text", QJsonObject{{"provider", "server"}});
        desk.settings->setCapabilities(all);
        desk.settings->setReadAloud({{"engine", "speech-dispatcher"}});
        return made;
    }

    // The page at a size, walked by the survey (desk.h): every problem found, by where.
    QStringList found;
    void look(Desk &desk, const QString &state) {
        QVERIFY2(desk.pictured(), qPrintable(state));
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        for (const QString &problem : std::as_const(survey.problems))
            if (!found.contains(where + " — " + problem))
                found << where + " — " + problem;
        desk.shot(QString("%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void init() { m_path = qgetenv("PATH"); }
    void cleanup() { qputenv("PATH", m_path); }
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        Secrets::useInstead(&m_secrets);
    }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    // Handwriting at an address of its own, with its own key, while answers
    // stay on the AI server; set up in its row.
    void handwritingHasAServerOfItsOwn() {
        FakeServer ai;
        ai.answering = serves;
        FakeServer ink;
        ink.answering = [](const FakeServer::Request &request) -> FakeServer::Reply {
            if (request.line.startsWith("GET"))
                return {.status = "200 OK", .body = R"({"data": [{"id": "qwen2.5vl"}]})"};
            return {.status = "200 OK", .body = R"({"model": "qwen2.5vl", "choices": [{"message": {"content": "OK"}}]})"};
        };
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, ai));
        QVERIFY(desk.show());
        QVERIFY(open(desk));
        desk.find("aiSettings")->setProperty("current", QString("handwriting"));
        QTRY_VERIFY(desk.shows("handwritingAddress"));
        QVERIFY(text(desk, "handwritingAddress").isEmpty());
        QObject *address = desk.find("handwritingAddress");
        address->setProperty("text", ink.url());
        QVERIFY(QMetaObject::invokeMethod(address, "editingFinished"));
        QCOMPARE(desk.settings->recognition().value("ink_url").toString(), ink.url());
        // Its model now is the server's own first, unless one is named.
        QObject *model = desk.find("handwritingModel");
        model->setProperty("text", "");
        QVERIFY(QMetaObject::invokeMethod(model, "editingFinished"));
        QTRY_VERIFY2(text(desk, "handwritingSummary").startsWith("the handwriting server at 127.0.0.1"),
                     qPrintable(text(desk, "handwritingSummary")));
        // Its key, kept in the keyring for its address, never in config.json.
        QTRY_VERIFY(desk.shows("handwritingKey"));
        desk.find("handwritingKey")->setProperty("text", "sk-ink");
        QVERIFY(QMetaObject::invokeMethod(desk.find("handwritingKey"), "accepted"));
        QCOMPARE(AiEndpoints::keptKey(ink.url(), m_secrets), QString("sk-ink"));
        QFile config(desk.dir.filePath("config.json"));
        QVERIFY(config.open(QIODevice::ReadOnly));
        QVERIFY(!config.readAll().contains("sk-ink"));

        // Its Test reads the drawn word there, with that key and the first
        // model it lists, and asks nothing of the AI server.
        const qsizetype asked = ai.received.size();
        press(desk, "handwritingTest");
        QTRY_VERIFY(ended(desk, Capability::Handwriting));
        QCOMPARE(outcome(desk, Capability::Handwriting).value("state").toString(), QString("passed"));
        QCOMPARE(outcome(desk, Capability::Handwriting).value("said").toString(), QString("OK"));
        QTRY_VERIFY(text(desk, "handwritingTestResult").startsWith(QString::fromUtf8("It read “OK” in")));
        QCOMPARE(ai.received.size(), asked);
        QVERIFY(!ink.received.isEmpty());
        const FakeServer::Request &read = ink.received.last();
        QVERIFY(read.line.startsWith("POST /v1/chat/completions"));
        QCOMPARE(read.json().value("model").toString(), QString("qwen2.5vl"));
        QVERIFY(read.head.contains("Bearer sk-ink"));
        QVERIFY(read.body.contains("image/png;base64"));

        // A hosted address is held to handwriting's own hosted choice.
        desk.settings->setRecognition({{"ink_url", "https://vision.example.com/v1"}});
        const auto status = AiRegistry::status(*desk.settings, Capability::Handwriting);
        QVERIFY(!status.available);
        QCOMPARE(status.why, CapabilityLack::HostedNotAllowed);

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A config.json from before: handwriting on the AI server's model, as it
    // was, and nothing of it rewritten.
    void anOlderConfigReadsOnUnchanged() {
        FakeServer ai;
        ai.answering = serves;
        QTemporaryDir dir;
        const QJsonObject recognition{{"ink_model", "gemma3"}, {"hosted", false}};
        QVERIFY(Fixtures::write(dir.filePath("config.json"),
                                QJsonDocument(QJsonObject{{"ai_provider", "openai"},
                                                          {"ai_endpoint", QJsonObject{{"preset", "custom"}, {"base_url", ai.url()}}},
                                                          {"recognition", recognition}})
                                    .toJson()));
        Settings settings(dir.filePath("config.json"));
        AiPrivacy::carryOver(settings);
        const auto unchanged = [&settings] {
            return !settings.recognition().contains("ink_url") && settings.recognition().value("ink_model") == "gemma3";
        };
        QVERIFY(unchanged());
        const auto status = AiRegistry::status(settings, Capability::Handwriting);
        QVERIFY2(status.available, qPrintable(status.failure.text()));
        QVERIFY2(status.provider.startsWith("the AI server at 127.0.0.1"), qPrintable(status.provider));
        QCOMPARE(status.model, QString("gemma3"));

        // Its row and Test go there too.
        AiCapabilityRows rows(&settings);
        rows.watch(true);
        QCOMPARE(rows.row(Capability::Handwriting).value("address").toString(), QString());
        rows.test("handwriting");
        QTRY_COMPARE(rows.tester()->outcome(Capability::Handwriting).value("state").toString(), QString("passed"));
        QCOMPARE(posted(ai).size(), 1);
        QCOMPARE(ai.received.last().json().value("model").toString(), QString("gemma3"));
        QVERIFY(unchanged());
    }

    // Every row's Test, through what the row uses, and what came back.
    void everyRowIsTestedThroughWhatItUses() {
        FakeServer server;
        server.answering = serves;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        QVERIFY(desk.show());
        QVERIFY(open(desk));
        const QList<std::pair<Capability, QString>> said{{Capability::Answers, "Working fine"},
                                                         {Capability::Quick, "Working fine"},
                                                         {Capability::Handwriting, "OK"},
                                                         {Capability::SpeechToText, "Hello, Omabook."},
                                                         {Capability::TextToSpeech, "This is how your books will be read aloud."},
                                                         {Capability::Embeddings, "3"}};
        for (const auto &[capability, words] : said) {
            const QString name = Capabilities::name(capability);
            desk.find("aiSettings")->setProperty("current", name);
            QTRY_VERIFY2(desk.shows(name + "Test"), qPrintable(name));
            press(desk, name + "Test");
            QTRY_VERIFY2(ended(desk, capability), qPrintable(name));
            const QVariantMap came = outcome(desk, capability);
            QVERIFY2(came.value("state") == "passed", qPrintable(name + ": " + came.value("message").toString()));
            QCOMPARE(came.value("said").toString(), words);
            QVERIFY(came.value("ms").toLongLong() >= 0);
            QTRY_VERIFY2(!text(desk, name + "TestResult").isEmpty(), qPrintable(name));
        }
        QTRY_VERIFY(text(desk, "embeddingsTestResult").startsWith("It made a vector of 3 numbers in"));
        // Answers and quick work each went with their own tier's request.
        const QStringList sent = posted(server);
        QVERIFY(sent.filter("/chat/completions").size() >= 3);
        QCOMPARE(sent.filter("/embeddings").size(), 1);
        QCOMPARE(sent.filter("/audio/transcriptions").size(), 1);
        // T on a row's head tests it too.
        desk.find("aiSettings")->setProperty("current", QString("embeddings"));
        desk.settle();
        qobject_cast<QQuickItem *>(desk.find("embeddingsRow"))->forceActiveFocus();
        QTest::keyClick(desk.window, Qt::Key_T);
        QTRY_COMPARE(posted(server).filter("/embeddings").size(), 2);

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Why a Test failed, as its code, and nothing sent where nothing may be.
    void aTestSaysWhyItFailed() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setCapabilities(Capabilities::fresh());
        settings.chooseAiProvider("openai");
        CapabilityTest test(&settings);
        const auto failing = [&](Capability capability, const QString &why) {
            test.run(capability, false);
            QTRY_VERIFY(test.outcome(capability).value("state") != "running");
            const QVariantMap came = test.outcome(capability);
            QCOMPARE(came.value("state").toString(), QString("failed"));
            QVERIFY2(came.value("why") == why, qPrintable(came.value("why").toString() + ": " + came.value("message").toString()));
        };
        const auto at = [&settings](const QString &url) {
            settings.setAiEndpoint({{"preset", "custom"}, {"base_url", url}});
            settings.setRecognition({{"ink_model", "gemma3"}});
            settings.setMeaning({{"model", "nomic-embed-text"}});
        };

        // A key it refuses.
        FakeServer refusing;
        refusing.answering = [](const FakeServer::Request &request) -> FakeServer::Reply {
            if (request.line.startsWith("GET"))
                return serves(request);
            return {.status = "401 Unauthorized", .body = R"({"error": {"message": "invalid api key"}})"};
        };
        at(refusing.url());
        for (const Capability capability : {Capability::Answers, Capability::Handwriting, Capability::Embeddings})
            failing(capability, "refused-key");

        // A model it hasn't.
        FakeServer modelless;
        modelless.answering = [](const FakeServer::Request &request) -> FakeServer::Reply {
            if (request.line.startsWith("GET"))
                return serves(request);
            return {.status = "404 Not Found", .body = "{\"error\": {\"message\": \"model gemma3 not found\"}}"};
        };
        at(modelless.url());
        for (const Capability capability : {Capability::Answers, Capability::Handwriting, Capability::Embeddings})
            failing(capability, "no-model");

        // Nothing at its address.
        quint16 port = 0;
        {
            const FakeServer gone;
            port = gone.serverPort();
        }
        at(QString("http://127.0.0.1:%1/v1").arg(port));
        for (const Capability capability : {Capability::Answers, Capability::Handwriting, Capability::Embeddings})
            failing(capability, "no-route");

        // A server that takes the request and never answers.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        at(QString("http://127.0.0.1:%1/v1").arg(silent.serverPort()));
        settings.setRecognition({{"ink_model", "gemma3"}});
        test.setTimeout(400ms);
        failing(Capability::Handwriting, "timeout");
        failing(Capability::Embeddings, "timeout");
        test.setTimeout(1min);

        // Hosted, where it isn't allowed: nothing is sent.
        FakeServer near;
        near.answering = serves;
        at(near.url());
        settings.setMeaning({{"model", "nomic-embed-text:cloud"}});
        failing(Capability::Embeddings, "hosted-not-allowed");
        QVERIFY(posted(near).isEmpty());
        // Unless the person said yes to this one test, which allows nothing after it.
        test.run(Capability::Embeddings, true);
        QTRY_COMPARE(test.outcome(Capability::Embeddings).value("state").toString(), QString("passed"));
        QCOMPARE(posted(near).filter("/embeddings").size(), 1);
        QVERIFY(!AiPrivacy::hostedAllowed(settings, Capability::Embeddings));
        QVERIFY(!settings.meaningHosted());

        // With the AI Off nothing is asked, hosted yes or no.
        settings.setAiProvider("off");
        const qsizetype asked = near.received.size();
        for (const Capability capability : Capabilities::all)
            failing(capability, "ai-off");
        test.run(Capability::Embeddings, true);
        QTRY_COMPARE(test.outcome(Capability::Embeddings).value("why").toString(), QString("ai-off"));
        QCOMPARE(near.received.size(), asked);
    }

    // A test that would go to someone else's computer asks first, in its row.
    void aHostedTestAsksFirst() {
        FakeServer server;
        server.answering = serves;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        desk.settings->setMeaning({{"model", "nomic-embed-text:cloud"}});
        QVERIFY(desk.show());
        QVERIFY(open(desk));
        desk.find("aiSettings")->setProperty("current", QString("embeddings"));
        QTRY_VERIFY(desk.shows("embeddingsTest"));
        press(desk, "embeddingsTest");
        QTRY_VERIFY(desk.shows("embeddingsTestAsk"));
        QVERIFY(outcome(desk, Capability::Embeddings).isEmpty());
        // No: nothing is sent.
        press(desk, "embeddingsTestCancel");
        QTRY_VERIFY(!desk.shows("embeddingsTestAsk"));
        QVERIFY(posted(server).filter("/embeddings").isEmpty());
        // Yes: this one test, and hosted stays not allowed.
        press(desk, "embeddingsTest");
        QTRY_VERIFY(desk.shows("embeddingsTestHosted"));
        press(desk, "embeddingsTestHosted");
        QTRY_VERIFY(ended(desk, Capability::Embeddings));
        QCOMPARE(outcome(desk, Capability::Embeddings).value("state").toString(), QString("passed"));
        QCOMPARE(posted(server).filter("/embeddings").size(), 1);
        QVERIFY(!AiPrivacy::hostedAllowed(*desk.settings, Capability::Embeddings));
        // With the AI Off there is no Test to press.
        desk.settings->setAiProvider("off");
        QTRY_VERIFY(!value(desk, "embeddingsTest", "enabled").toBool());

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // What each row is used for: in every catalog, and on the page in the
    // person's language.
    void theUsedForLinesAreInEveryCatalog() {
        for (const char *language : {"en", "de", "es-MX", "fr", "nl"}) {
            QFile file(QString(":/i18n/%1.json").arg(language));
            QVERIFY2(file.open(QIODevice::ReadOnly), language);
            const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
            for (const QString &line : usedFor) {
                QVERIFY2(catalog.contains(line), qPrintable(QString(language) + ": " + line));
                if (QByteArray(language) != "en")
                    QVERIFY2(catalog.value(line).toString() != line, qPrintable(QString(language) + ": " + line));
            }
        }
        Desk desk("de", {}, false);
        QVERIFY(desk.show());
        QVERIFY(open(desk));
        QFile german(":/i18n/de.json");
        QVERIFY(german.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(german.readAll()).object();
        const QStringList rows{"answers", "quick", "handwriting", "speech-to-text", "text-to-speech", "embeddings"};
        for (qsizetype at = 0; at < rows.size(); ++at)
            QTRY_COMPARE(text(desk, rows.at(at) + "UsedFor"), catalog.value(usedFor.at(at)).toString());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The page with every row tested (one asking first), at every size, in a
    // long language, under fingers and right to left.
    void thePageWithItsTestsAtEverySize() {
        FakeServer server;
        server.answering = serves;
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            QVERIFY(fakeProviders(desk, server));
            desk.settings->setMeaning({{"model", "nomic-embed-text:cloud"}});
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QVERIFY(open(desk));
            for (const Capability capability : {Capability::Answers, Capability::Quick, Capability::Handwriting, Capability::SpeechToText,
                                                Capability::TextToSpeech, Capability::Embeddings}) {
                desk.aiAgent->capabilities()->test(Capabilities::name(capability));
                QTRY_VERIFY(ended(desk, capability));
            }
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                desk.quicken();
                for (const char *row : {"handwriting", "speech-to-text", "text-to-speech", "embeddings"}) {
                    desk.find("aiSettings")->setProperty("current", QString(row));
                    desk.settle();
                    look(desk, QString("tested-") + row);
                }
                press(desk, "embeddingsTest");
                QTRY_VERIFY(desk.shows("embeddingsTestAsk"));
                desk.settle();
                look(desk, "asking");
                press(desk, "embeddingsTestCancel");
            }
            QVERIFY(desk.closePopups());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = std::exchange(found, {});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }
};

QTEST_MAIN(CapabilityTestTests)
#include "capabilitytest_tests.moc"
