// A book's graph built with a model on an Ollama server (#204), against a
// stand-in that answers the way local models do: thinking before the JSON,
// an answer cut off where the room ran out, an address typed without /v1.
// Ollama keeps a request to a few thousand tokens unless it is told more,
// and cut a section of a book down to its end, instructions and all, so the
// model wrote about the book instead of the JSON asked for.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "ai.h"
#include "aiendpoint.h"
#include "fakeaiserver.h"
#include "graph.h"
#include "graphbuilder.h"
#include "library.h"
#include "ollamaprovider.h"
#include "prompts.h"
#include "secretstore.h"
#include "settings.h"

namespace {

// What a model answers for a page of Alice in Wonderland.
const QByteArray aliceAnswer =
    R"({"nodes": [{"kind": "character", "name": "Alice", "summary": "A girl who follows a rabbit.", "section": 0, "quote": "Alice was beginning"},)"
    R"( {"kind": "character", "name": "White Rabbit", "summary": "A rabbit with a watch.", "section": 0, "quote": "a White Rabbit"}],)"
    R"( "edges": [{"from": "Alice", "to": "White Rabbit", "relation": "follows", "section": 0, "quote": "ran across the field after it"}]})";

const QString alicePage = "Alice was beginning to get very tired of sitting by her sister on the bank. Suddenly a White Rabbit "
                          "with pink eyes ran close by her, and she ran across the field after it.";

// Ollama's own stream: one JSON object to a line, the last saying why it stopped.
QByteArray ndjson(const QList<QJsonObject> &messages, const QString &why = "stop") {
    QByteArray body;
    for (const QJsonObject &message : messages)
        body += QJsonDocument(QJsonObject{{"model", "gemma4:e2b"}, {"message", message}, {"done", false}}).toJson(QJsonDocument::Compact)
                + "\n";
    const QJsonObject last{
        {"model", "gemma4:e2b"}, {"message", QJsonObject{{"role", "assistant"}, {"content", ""}}}, {"done", true}, {"done_reason", why}};
    return body + QJsonDocument(last).toJson(QJsonDocument::Compact) + "\n";
}

QJsonObject said(const QString &content, const QString &thinking = {}) {
    QJsonObject message{{"role", "assistant"}, {"content", content}};
    if (!thinking.isEmpty())
        message.insert("thinking", thinking);
    return message;
}

// The answer cut into pieces as a model streams it.
QList<QJsonObject> inPieces(const QByteArray &answer, int size = 40) {
    QList<QJsonObject> pieces;
    for (qsizetype at = 0; at < answer.size(); at += size)
        pieces << said(QString::fromUtf8(answer.mid(at, size)));
    return pieces;
}

// The root of the stand-in, as a person types Ollama's address.
QString rootOf(const FakeServer &fake) { return QString("http://127.0.0.1:%1").arg(fake.serverPort()); }

AiServer ollamaAt(const FakeServer &fake) {
    return {.preset = "ollama", .name = "Ollama", .baseUrl = rootOf(fake) + "/v1", .key = {}, .keyFile = {}, .model = "gemma4:e2b"};
}

AiRequest asking(const QString &system, const QString &text, AiRequest::Tier tier = AiRequest::Main) {
    return {.system = system, .messages = {{"user", text}}, .tier = tier, .model = {}, .prose = false, .format = {}};
}

FakeServer::Reply replying(const QByteArray &body, const QByteArray &status = "200 OK") { return {.status = status, .body = body}; }

struct Outcome {
    QString answer, failure;
};

Outcome ask(AiProvider &provider, const AiRequest &request) {
    Outcome outcome;
    bool over = false;
    QObject context;
    QObject::connect(&provider, &AiProvider::delta, &context, [&](const QString &text) { outcome.answer += text; });
    QObject::connect(&provider, &AiProvider::finished, &context, [&] { over = true; });
    QObject::connect(&provider, &AiProvider::failed, &context, [&](const Failure &why) {
        outcome.failure = why.text();
        over = true;
    });
    provider.ask(request);
    if (!QTest::qWaitFor([&] { return over; }, 10000))
        outcome.failure = "no answer in time";
    return outcome;
}

// Settings that send quick work to a model on the stand-in, at this address.
void useOllama(Settings &settings, const QString &address) {
    settings.setAiProvider("openai");
    settings.setAiEndpoint({{"preset", "ollama"}, {"base_url", address}});
    settings.setAiModelFor(AiEndpoints::modelsKey(settings), "quick", "gemma4:e2b");
}

} // namespace

class OllamaTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { Secrets::useInstead(&m_secrets); }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    // As #170's Test should: Ollama's address with /v1 or without it.
    void anAddressWithOrWithoutV1Reaches_data() {
        QTest::addColumn<QString>("typed");
        QTest::newRow("the server's address") << "";
        QTest::newRow("with a slash") << "/";
        QTest::newRow("with /v1") << "/v1";
        QTest::newRow("with /v1/") << "/v1/";
    }
    void anAddressWithOrWithoutV1Reaches() {
        QFETCH(QString, typed);
        QTemporaryDir dir;
        FakeServer fake;
        fake.answering = [](const FakeServer::Request &) { return replying(ndjson({said("ok")})); };
        Settings settings(dir.filePath("config.json"));
        useOllama(settings, rootOf(fake) + typed);
        QCOMPARE(AiEndpoints::baseUrl(settings), rootOf(fake) + "/v1");
        QString why;
        const std::unique_ptr<AiProvider> provider = makeAiProvider(settings, &why, dir.filePath("no-omarchy"));
        QVERIFY2(provider, qPrintable(why));
        const Outcome outcome = ask(*provider, asking("Be brief.", "Hi", AiRequest::Quick));
        QCOMPARE(outcome.failure, QString());
        QCOMPARE(outcome.answer, QString("ok"));
        QVERIFY2(fake.received.last().line.startsWith("POST /api/chat "), fake.received.last().line.constData());
        QCOMPARE(fake.received.last().json().value("model").toString(), QString("gemma4:e2b"));
    }

    void anotherServersAddressKeepsItsPath() {
        QCOMPARE(AiEndpoints::apiUrl("https://openrouter.ai/api/v1/"), QString("https://openrouter.ai/api/v1"));
        QCOMPARE(AiEndpoints::apiUrl("http://desktop:8080/"), QString("http://desktop:8080/v1"));
        QCOMPARE(AiEndpoints::apiUrl(" http://desktop:8080/openai "), QString("http://desktop:8080/openai"));
        QCOMPARE(AiEndpoints::apiUrl(""), QString());
    }

    // The whole section and the instructions before it fit in the room asked for.
    void aSectionGetsTheRoomItNeeds() {
        FakeServer fake;
        fake.answering = [](const FakeServer::Request &) { return replying(ndjson(inPieces(aliceAnswer))); };
        OllamaProvider provider(ollamaAt(fake));
        const QString section = QString(alicePage + "\n").repeated(160); // some 24,000 characters, as GraphBuild::chunkSize
        const AiRequest request = Prompts::graphExtraction({}, {}, "<section number=\"0\">\n" + section + "</section>");
        QCOMPARE(ask(provider, request).failure, QString());

        const QJsonObject sent = fake.received.last().json();
        const qsizetype characters = request.system.size() + request.messages.first().text.size();
        const int room = sent.value("options").toObject().value("num_ctx").toInt();
        QVERIFY2(room >= characters / 3 + 4096, qPrintable(QString::number(room)));
        QCOMPARE(room & (room - 1), 0); // a power of two, so sections of a size share one
        QCOMPARE(sent.value("messages").toArray().first().toObject().value("content").toString(), request.system);
        // Held to the shape the graph reads.
        QCOMPARE(sent.value("format").toObject(), request.format);
        QVERIFY(sent.value("format").toObject().value("properties").toObject().contains("nodes"));

        // A short question still gets more than Ollama's own few thousand.
        QCOMPARE(OllamaProvider::roomFor(asking("Be brief.", "Hi")), 8192);
        QCOMPARE(OllamaProvider::roomFor(asking({}, QString(1000000, 'x'))), OllamaProvider::mostRoom);
    }

    // gemma4 and qwen3 think first: apart, or in the answer between <think> tags.
    void aThinkingModelsJsonBuildsTheGraph_data() {
        QTest::addColumn<QByteArray>("stream");
        QList<QJsonObject> apart{said("", "The user wants nodes. Alice is {a character}."), said("", " The rabbit too.")};
        apart += inPieces(aliceAnswer);
        QTest::newRow("thinking apart") << ndjson(apart);
        QTest::newRow("thinking in the answer")
            << ndjson(inPieces("<think>\nAlice {is} the girl; the rabbit runs.\n</think>\n\n" + aliceAnswer, 7));
        QTest::newRow("thinking in the answer, then a fence")
            << ndjson(inPieces("<think>Let me list them: {Alice}.</think>```json\n" + aliceAnswer + "\n```", 13));
    }
    void aThinkingModelsJsonBuildsTheGraph() {
        QFETCH(QByteArray, stream);
        FakeServer fake;
        fake.answering = [&](const FakeServer::Request &) { return replying(stream); };
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "Alice's Adventures in Wonderland";
        meta.authors = QStringList{"Lewis Carroll"};
        const qint64 book = library.addEntry(meta, true).id;
        const QList<GraphBuild::Passage> passages{{0, 0, 1, alicePage}};

        OllamaProvider provider(ollamaAt(fake));
        const Outcome outcome = ask(provider, GraphBuild::request(library, book, passages));
        QCOMPARE(outcome.failure, QString());
        QVERIFY2(!outcome.answer.contains("think") && !outcome.answer.contains("rabbit runs"), qPrintable(outcome.answer));
        const GraphBuild::Applied applied = GraphBuild::apply(library, book, passages, outcome.answer);
        QVERIFY2(applied.understood, qPrintable(outcome.answer));
        QCOMPARE(applied.nodes, 2);
        QCOMPARE(applied.edges, 1);
        QVERIFY(Graph(library).named({}, "character", "White Rabbit") > 0);
    }

    // An answer that ran out of room is asked again with twice as much, and
    // only the whole one is given.
    void anAnswerCutOffIsAskedAgainWithMoreRoom() {
        FakeServer fake;
        fake.replies << replying(ndjson(inPieces(aliceAnswer.left(90)), "length")) << replying(ndjson(inPieces(aliceAnswer)));
        OllamaProvider provider(ollamaAt(fake));
        const AiRequest request = Prompts::graphExtraction({}, {}, alicePage);
        const Outcome outcome = ask(provider, request);
        QCOMPARE(outcome.failure, QString());
        QCOMPARE(outcome.answer, QString::fromUtf8(aliceAnswer));
        QCOMPARE(fake.received.size(), 2);
        const int first = fake.received.first().json().value("options").toObject().value("num_ctx").toInt();
        QCOMPARE(fake.received.last().json().value("options").toObject().value("num_ctx").toInt(), 2 * first);

        // Where there is no more room to give, what came is what there is.
        fake.replies << replying(ndjson({said("{\"nodes\": [")}, "length"));
        AiRequest huge = request;
        huge.messages.first().text = QString(1000000, 'x');
        QCOMPARE(ask(provider, huge).answer, QString("{\"nodes\": ["));
        QCOMPARE(fake.received.size(), 3);

        // Prose is shown as it is written, so it is never asked twice.
        fake.replies << replying(ndjson({said("Alice falls")}, "length"));
        QCOMPARE(ask(provider, asking("Tell.", "What happens?")).answer, QString("Alice falls"));
        QCOMPARE(fake.received.size(), 4);
        QVERIFY(!fake.received.last().json().contains("format"));
    }

    void aMissingModelSaysSo() {
        FakeServer fake;
        fake.replies << replying("{\"error\":\"model 'gemma4:e2b' not found, try pulling it first\"}", "404 Not Found");
        OllamaProvider provider(ollamaAt(fake));
        const Outcome outcome = ask(provider, asking({}, "Hi", AiRequest::Quick));
        QVERIFY2(outcome.failure.contains("gemma4:e2b"), qPrintable(outcome.failure));
    }

    // Any other OpenAI-compatible server, hosted ones among them, is asked as
    // before: Chat Completions, with nothing of Ollama's.
    void anotherServerIsAskedAsBefore() {
        QTemporaryDir dir;
        FakeServer fake;
        fake.answering = [](const FakeServer::Request &) {
            return replying("data: {\"choices\":[{\"delta\":{\"content\":\"{}\"}}]}\n\ndata: [DONE]\n\n");
        };
        Settings settings(dir.filePath("config.json"));
        settings.setAiProvider("openai");
        settings.setAiEndpoint({{"preset", "custom"}, {"base_url", fake.url()}});
        settings.setAiModelFor(AiEndpoints::modelsKey(settings), "quick", "small");
        QString why;
        const std::unique_ptr<AiProvider> provider = makeAiProvider(settings, &why, dir.filePath("no-omarchy"));
        QVERIFY2(provider, qPrintable(why));
        QCOMPARE(ask(*provider, Prompts::graphExtraction({}, {}, alicePage)).answer, QString("{}"));
        QVERIFY(fake.received.last().line.startsWith("POST /v1/chat/completions "));
        const QJsonObject sent = fake.received.last().json();
        QVERIFY(!sent.contains("format") && !sent.contains("options") && !sent.contains("response_format"));
    }

private:
    SessionSecrets m_secrets;
};

QTEST_MAIN(OllamaTests)
#include "ollama_tests.moc"
