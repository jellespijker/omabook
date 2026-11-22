// A second server per capability, used when the first can't be reached
// (#174): against two fake servers, the first away and then back, for
// meaning, handwriting, speech, voices and answers; quick work waiting, and
// the knowledge graph catching up once its server is back; each capability's
// default; a fallback that is someone else's computer needing its own
// allowance; the AI Off; a first that answers with an error kept; and the AI
// page's row, which shows the fallback in use and tests it.

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <chrono>
#include <memory>
#include <optional>

#include "aicapabilityrows.h"
#include "aifallback.h"
#include "aiprivacy.h"
#include "airegistry.h"
#include "embeddings.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "graphbuilder.h"
#include "library.h"
#include "secretstore.h"
#include "serverreach.h"
#include "settings.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// Answers POST /embeddings with a vector for each input, and chat with a word.
FakeServer::Reply answered(const FakeServer::Request &request) {
    if (request.line.contains("/embeddings")) {
        QJsonArray data;
        const QJsonArray input = request.json().value("input").toArray();
        for (qsizetype at = 0; at < input.size(); ++at)
            data.append(QJsonObject{{"index", at}, {"embedding", QJsonArray{3.0, 4.0}}});
        return {.status = "200 OK",
                .body = QJsonDocument(QJsonObject{{"data", data}}).toJson(QJsonDocument::Compact),
                .type = "application/json"};
    }
    if (request.json().value("stream").toBool())
        return {.status = "200 OK",
                .body = "data: "
                        + QJsonDocument(
                              QJsonObject{{"choices",
                                           QJsonArray{QJsonObject{{"index", 0},
                                                                  {"delta", QJsonObject{{"content", "{\"nodes\": [], \"edges\": []}"}}}}}}})
                              .toJson(QJsonDocument::Compact)
                        + "\n\ndata: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
                .type = "text/event-stream"};
    const QJsonObject message{{"role", "assistant"}, {"content", "OK"}};
    return {.status = "200 OK",
            .body = QJsonDocument(QJsonObject{{"model", "llava"}, {"choices", QJsonArray{QJsonObject{{"message", message}}}}})
                        .toJson(QJsonDocument::Compact),
            .type = "application/json"};
}

// A server that can be away and back on the same address.
struct Server {
    FakeServer fake;
    quint16 port = 0;
    QString url;

    explicit Server(bool there = true) {
        fake.answering = answered;
        port = fake.serverPort();
        url = fake.url();
        if (!there)
            away();
    }
    void away() {
        fake.close();
        ServerReach::forget();
    }
    void back() {
        QVERIFY(fake.listen(QHostAddress::LocalHost, port));
        ServerReach::forget();
    }
    [[nodiscard]] qsizetype asked(const QByteArray &path) const {
        return std::ranges::count_if(fake.received, [&path](const FakeServer::Request &one) { return one.line.contains(path); });
    }
};

struct Bench {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};

    Bench() { AiPrivacy::carryOver(settings); }
    void fallBackTo(Capability capability, const QString &url, const QString &model = {}) {
        AiFallback::choose(settings, capability, {{u"to"_s, u"server"_s}, {u"url"_s, url}, {u"model"_s, model}});
    }
    // Answers and quick work from a server at this address, with these models.
    void answerFrom(const QString &url) {
        settings.setAiProvider("openai");
        settings.setAiEndpoint({{"preset", "custom"}, {"base_url", url}});
        settings.setAiModelFor(AiEndpoints::modelsKey(settings), "main", "big");
        settings.setAiModelFor(AiEndpoints::modelsKey(settings), "quick", "small");
    }
};

[[nodiscard]] AiRequest asking(AiRequest::Tier tier) {
    return {.system = {}, .messages = {{u"user"_s, u"Who is Ishmael?"_s}}, .tier = tier, .model = {}, .prose = false};
}

struct Outcome {
    std::optional<Failure> failure;
    QList<Failure> notes;
};
[[nodiscard]] Outcome askOf(AiProvider &provider, const AiRequest &request) {
    QSignalSpy finished(&provider, &AiProvider::finished);
    QSignalSpy failed(&provider, &AiProvider::failed);
    QSignalSpy noted(&provider, &AiProvider::noted);
    provider.ask(request);
    if (!QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 5000))
        return {.failure = Failure::said(u"no answer"_s), .notes = {}};
    QCoreApplication::processEvents();
    Outcome outcome;
    if (!failed.isEmpty())
        outcome.failure = failed.first().first().value<Failure>();
    for (const auto &one : noted)
        outcome.notes << one.first().value<Failure>();
    return outcome;
}

} // namespace

class AiFallbackTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;

private slots:
    void initTestCase() { Secrets::useInstead(&m_secrets); }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }
    void init() { ServerReach::holdFor(0ms); }

    // Meaning, handwriting, speech and voices fall back to this computer by
    // themselves; quick work waits; answers go elsewhere only where chosen.
    void eachCapabilityHasItsDefault() {
        Bench bench;
        for (const Capability each :
             {Capability::Handwriting, Capability::SpeechToText, Capability::TextToSpeech, Capability::Embeddings}) {
            QCOMPARE(AiFallback::rule(each), AiFallback::Rule::FallsBack);
            QCOMPARE(AiFallback::chosen(bench.settings, each).to, AiFallback::To::ThisComputer);
        }
        QCOMPARE(AiFallback::rule(Capability::Quick), AiFallback::Rule::Waits);
        QCOMPARE(AiFallback::chosen(bench.settings, Capability::Quick).to, AiFallback::To::None);
        QCOMPARE(AiFallback::rule(Capability::Answers), AiFallback::Rule::IfChosen);
        QCOMPARE(AiFallback::chosen(bench.settings, Capability::Answers).to, AiFallback::To::None);

        // Quick work has none, whatever is written for it.
        bench.fallBackTo(Capability::Quick, u"http://127.0.0.1:9/v1"_s);
        QCOMPARE(AiFallback::chosen(bench.settings, Capability::Quick).to, AiFallback::To::None);
        // This computer's Ollama, for a first elsewhere: never for one that is this computer already.
        const auto here = AiFallback::instead(bench.settings, Capability::Embeddings, u"http://127.0.0.1:9/v1"_s, u"nomic-embed-text"_s);
        QVERIFY(!here);
        AiFallback::choose(bench.settings, Capability::Embeddings, {{u"to"_s, u"none"_s}});
        QCOMPARE(AiFallback::chosen(bench.settings, Capability::Embeddings).to, AiFallback::To::None);
    }

    // The first away: the fallback, with the same model, said; the first back:
    // the first again, by itself once what was found of it is no longer held.
    void meaningGoesToItsFallbackAndBack() {
        Server first(false);
        Server second;
        Bench bench;
        bench.settings.setMeaning({{"url", first.url}, {"model", "nomic-embed-text"}});
        bench.fallBackTo(Capability::Embeddings, second.url, u"another-model"_s);

        auto route = AiRegistry::embeddings(bench.settings);
        QVERIFY2(route, qPrintable(Fixtures::whyNot(route)));
        QCOMPARE(route->server.baseUrl, second.url);
        QCOMPARE(route->insteadOf, u"127.0.0.1"_s);
        // Vectors of two models can't be searched together: the first's, on either.
        QCOMPARE(route->model, u"nomic-embed-text"_s);
        const auto made = Embeddings::embed(*route, {u"Call me Ishmael."_s}, Embeddings::Use::Question, 5s);
        QVERIFY2(made, qPrintable(Fixtures::whyNot(made)));
        QCOMPARE(second.fake.received.size(), 1);
        QCOMPARE(second.fake.received.first().json().value("model").toString(), u"nomic-embed-text"_s);

        const QJsonObject status = AiRegistry::status(bench.settings, Capability::Embeddings).toJson();
        QCOMPARE(status.value("fallback").toBool(), true);
        QCOMPARE(status.value("first").toString(), u"127.0.0.1"_s);

        // Back: held a moment, then tried again and used.
        ServerReach::holdFor(150ms);
        QVERIFY(AiRegistry::embeddings(bench.settings)->server.baseUrl == second.url);
        first.fake.listen(QHostAddress::LocalHost, first.port);
        QTest::qWait(200);
        route = AiRegistry::embeddings(bench.settings);
        QCOMPARE(route->server.baseUrl, first.url);
        QVERIFY(route->insteadOf.isEmpty());
        QCOMPARE(AiRegistry::status(bench.settings, Capability::Embeddings).toJson().value("fallback").toBool(), false);
    }

    // A server that answers with an error was reached: it isn't passed over.
    void aFirstThatAnswersWithAnErrorIsKept() {
        Server first;
        first.fake.answering = [](const FakeServer::Request &) {
            return FakeServer::Reply{.status = "500 Internal Server Error", .body = "{}", .type = "application/json"};
        };
        Server second;
        Bench bench;
        bench.settings.setMeaning({{"url", first.url}});
        bench.fallBackTo(Capability::Embeddings, second.url);
        const auto route = AiRegistry::embeddings(bench.settings);
        QCOMPARE(route->server.baseUrl, first.url);
        QVERIFY(!Embeddings::embed(*route, {u"Call me Ishmael."_s}, Embeddings::Use::Question, 5s));
        QCOMPARE(second.fake.received.size(), 0);
    }

    // Someone else's computer as a fallback needs its own allowance, and is
    // never used for one who kept to their own; with the AI Off nothing is.
    void aHostedFallbackNeedsItsOwnAllowance() {
        Server first(false);
        Bench bench;
        const QString hosted = u"https://ai.example.com/v1"_s;
        bench.fallBackTo(Capability::Handwriting, hosted, u"qwen3-vl"_s);
        QVERIFY(!AiFallback::instead(bench.settings, Capability::Handwriting, first.url, u"llava"_s));
        AiFallback::choose(bench.settings, Capability::Handwriting, {{u"hosted"_s, true}});
        const auto used = AiFallback::instead(bench.settings, Capability::Handwriting, first.url, u"llava"_s);
        QVERIFY(used);
        QVERIFY(used->hosted);
        QCOMPARE(used->model, u"qwen3-vl"_s);
        QVERIFY(!AiFallback::instead(bench.settings, Capability::Handwriting, first.url, u"llava"_s, false));
        bench.settings.setAiProvider("off");
        QVERIFY(!AiFallback::instead(bench.settings, Capability::Handwriting, first.url, u"llava"_s));
    }

    // Handwriting, speech and voices take their fallback in place of a server away.
    void recognitionAndVoicesFallBack() {
        Server first(false);
        Server second;
        Bench bench;
        bench.settings.setRecognition({{"ink_url", first.url},
                                       {"ink_model", "llava"},
                                       {"transcription_url", first.url},
                                       {"whisper_program", "/nonexistent/whisper"}});
        bench.settings.setReadAloud({{"engine", "server"}, {"url", first.url}});
        for (const Capability each : {Capability::Handwriting, Capability::SpeechToText, Capability::TextToSpeech})
            bench.fallBackTo(each, second.url);

        for (const Capability each : {Capability::Handwriting, Capability::SpeechToText, Capability::TextToSpeech}) {
            const CapabilityStatus status = AiRegistry::status(bench.settings, each);
            QVERIFY2(status.available, qPrintable(Capabilities::name(each) + u": "_s + status.failure.text()));
            QCOMPARE(status.insteadOf, u"127.0.0.1"_s);
        }
        const auto ways = AiRegistry::recognizers(bench.settings, Recognition::Kind::Audio, true);
        QVERIFY(ways && !ways->empty());
        QVERIFY(AiRegistry::voice(bench.settings));

        // The row's Test of handwriting goes to the fallback, which reads it.
        AiCapabilityRows rows(&bench.settings);
        rows.watch(true);
        rows.test(u"handwriting"_s);
        QVERIFY(QTest::qWaitFor([&] { return rows.tester()->outcome(Capability::Handwriting).value("state") != u"running"_s; }, 10000));
        QCOMPARE(rows.tester()->outcome(Capability::Handwriting).value("state").toString(), u"passed"_s);
        QCOMPARE(second.asked("/chat/completions"), 1);
    }

    // Answers go to their fallback only where chosen, saying so with each
    // answer; quick work waits for the server rather than go there.
    void answersFallBackOnlyWhereChosenAndQuickWaits() {
        Server first(false);
        Server second;
        Bench bench;
        bench.answerFrom(first.url);
        QVERIFY(AiRegistry::status(bench.settings, Capability::Answers).insteadOf.isEmpty());
        QVERIFY(AiRegistry::status(bench.settings, Capability::Quick).available);

        bench.fallBackTo(Capability::Answers, second.url, u"llama3.2:3b"_s);
        const CapabilityStatus answers = AiRegistry::status(bench.settings, Capability::Answers);
        QVERIFY(answers.available);
        QCOMPARE(answers.insteadOf, u"127.0.0.1"_s);
        QCOMPARE(answers.model, u"llama3.2:3b"_s);
        const CapabilityStatus quick = AiRegistry::status(bench.settings, Capability::Quick);
        QVERIFY(!quick.available);
        QCOMPARE(quick.why, CapabilityLack::Unreachable);
        QCOMPARE(quick.failure.code(), Failure::Code::AiQuickWaits);

        auto provider = AiRegistry::answers(bench.settings);
        QVERIFY2(provider, qPrintable(Fixtures::whyNot(provider)));
        const Outcome main = askOf(**provider, asking(AiRequest::Main));
        QVERIFY2(!main.failure, qPrintable(main.failure ? main.failure->text() : QString()));
        QCOMPARE(main.notes.size(), 1);
        QCOMPARE(main.notes.first().code(), Failure::Code::AiAnsweredInstead);
        QCOMPARE(second.fake.received.last().json().value("model").toString(), u"llama3.2:3b"_s);
        const Outcome waited = askOf(**provider, asking(AiRequest::Quick));
        QVERIFY(waited.failure);
        QCOMPARE(waited.failure->code(), Failure::Code::AiQuickWaits);
        QCOMPARE(second.asked("/chat/completions"), 1);
    }

    // The knowledge graph waits for its server: nothing is marked as built,
    // and once the server is back the same pages go to it.
    void graphWorkWaitsAndCatchesUp() {
        Server first(false);
        Server second;
        Bench bench;
        bench.answerFrom(first.url);
        bench.fallBackTo(Capability::Answers, second.url);
        Library library(bench.dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::write(bench.dir.filePath("Moby.txt"), "Call me Ishmael. Some years ago, never mind how long precisely."));
        const qint64 book = library.add(bench.dir.filePath("Moby.txt")).id;
        QVERIFY(book > 0);
        const QList<GraphBuild::Passage> passages{{.section = 0, .from = 0, .to = 1, .text = u"Call me Ishmael. Some years ago."_s}};

        GraphBuilder builder(AiRegistry::answerer(bench.settings), [&library] { return &library; });
        QSignalSpy failed(&builder, &GraphBuilder::failed);
        QSignalSpy grown(&builder, &GraphBuilder::grown);
        builder.grow(book, passages);
        QVERIFY(QTest::qWaitFor([&] { return !failed.isEmpty(); }, 5000));
        QVERIFY(!GraphBuild::builtTo(library, book));
        QCOMPARE(second.fake.received.size(), 0);

        first.back();
        QVERIFY(QTest::qWaitFor([&] { return !builder.busy(); }, 5000));
        builder.grow(book, passages);
        QVERIFY(QTest::qWaitFor([&] { return !grown.isEmpty(); }, 5000));
        QVERIFY(GraphBuild::builtTo(library, book));
        QVERIFY(first.asked("/chat/completions") >= 1);
        QCOMPARE(second.fake.received.size(), 0);
    }

    // The AI page's row: its fallback chosen there, shown while in use, and
    // tested on its own, as though the first were away.
    void theRowShowsAndTestsItsFallback() {
        Server first;
        Server second;
        Bench bench;
        bench.settings.setMeaning({{"url", first.url}});
        AiCapabilityRows rows(&bench.settings);
        rows.watch(true);
        rows.setFallback(u"embeddings"_s, {{u"to"_s, u"server"_s}, {u"url"_s, second.url + u'/'}});
        QVariantMap row = rows.row(Capability::Embeddings);
        QCOMPARE(row.value("fallbackChoice").toMap().value("url").toString(), second.url);
        QCOMPARE(row.value("fallbackChoice").toMap().value("rule").toString(), u"falls-back"_s);
        QCOMPARE(row.value("fallback").toBool(), false);
        QCOMPARE(rows.row(Capability::Quick).value("fallbackChoice").toMap().value("rule").toString(), u"waits"_s);

        rows.test(u"embeddings"_s, false, true);
        QVERIFY(
            QTest::qWaitFor([&] { return rows.tester()->outcome(Capability::Embeddings, true).value("state") != u"running"_s; }, 10000));
        QCOMPARE(rows.tester()->outcome(Capability::Embeddings, true).value("state").toString(), u"passed"_s);
        QCOMPARE(second.asked("/embeddings"), 1);
        QCOMPARE(first.fake.received.size(), 0);

        first.away();
        row = rows.row(Capability::Embeddings);
        QCOMPARE(row.value("fallback").toBool(), true);
        QCOMPARE(row.value("first").toString(), u"127.0.0.1"_s);

        // Nothing to try: the Test says so.
        rows.setFallback(u"embeddings"_s, {{u"to"_s, u"none"_s}});
        rows.test(u"embeddings"_s, false, true);
        QVERIFY(QTest::qWaitFor([&] { return rows.tester()->outcome(Capability::Embeddings, true).value("state") != u"running"_s; }, 5000));
        QCOMPARE(rows.tester()->outcome(Capability::Embeddings, true).value("why").toString(), u"no-route"_s);
    }
};

QTEST_MAIN(AiFallbackTests)
#include "aifallback_tests.moc"
