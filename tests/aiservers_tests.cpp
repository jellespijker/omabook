#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "ai.h"
#include "aicall.h"
#include "aiendpoint.h"
#include "fixtures.h"
#include "markupfilter.h"
#include "omarchylocal.h"
#include "openaiprovider.h"
#include "secretstore.h"
#include "settings.h"
#include "thinkingfilter.h"

namespace {

using Fixtures::script;

// A server that speaks the OpenAI-compatible API, standing in for Ollama,
// Omarchy's local models or a hosted service: it answers each request with
// the next reply in its queue, and keeps every request it was sent.
class FakeServer : public QTcpServer {
public:
    struct Reply {
        QByteArray status = "200 OK";
        QByteArray body;
        QByteArray type = "text/event-stream";
    };
    struct Request {
        QByteArray line;    // "POST /v1/chat/completions HTTP/1.1"
        QByteArray headers; // lower case
        QJsonObject body;
    };
    QList<Reply> replies;
    QList<Request> received;

    FakeServer() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                *buffer += socket->readAll();
                const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                const QByteArray head = buffer->left(headerEnd);
                const QByteArray lower = head.toLower();
                const qsizetype at = lower.indexOf("content-length:");
                const qsizetype length = at < 0 ? 0 : lower.mid(at + 15, lower.indexOf("\r\n", at) - at - 15).trimmed().toLongLong();
                if (buffer->size() < headerEnd + 4 + length)
                    return;
                received.append({.line = head.left(head.indexOf("\r\n")),
                                 .headers = lower,
                                 .body = QJsonDocument::fromJson(buffer->mid(headerEnd + 4, length)).object()});
                const Reply reply = replies.isEmpty()
                                        ? Reply{.status = "500 Internal Server Error", .body = "{}", .type = "application/json"}
                                        : replies.takeFirst();
                socket->write("HTTP/1.1 " + reply.status + "\r\nContent-Type: " + reply.type + "\r\nContent-Length: "
                              + QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body);
                socket->disconnectFromHost();
            });
        });
    }

    QString url() const { return QString("http://127.0.0.1:%1/v1").arg(serverPort()); }
    void answer(const QList<QByteArray> &pieces) {
        replies.append({.status = "200 OK", .body = stream(pieces), .type = "text/event-stream"});
    }
    void refuse(const QByteArray &status, const QByteArray &json) {
        replies.append({.status = status, .body = json, .type = "application/json"});
    }
    void list(const QStringList &models) {
        QJsonArray data;
        for (const QString &model : models)
            data.append(QJsonObject{{"id", model}, {"object", "model"}});
        replies.append({.status = "200 OK",
                        .body = QJsonDocument(QJsonObject{{"object", "list"}, {"data", data}}).toJson(),
                        .type = "application/json"});
    }

    // An answer as Chat Completions streams it: one chunk per piece, a
    // thinking model's reasoning among them, and [DONE] at the end.
    static QByteArray stream(const QList<QByteArray> &pieces) {
        QByteArray body = "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"hmm\"}}]}\n\n"
                          "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning\":\"let me think\"}}]}\n\n";
        for (const QByteArray &piece : pieces)
            body += "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + piece + "\"}}]}\n\n";
        return body + "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    }
};

AiServer serverAt(const FakeServer &fake, const QString &key = {}) {
    return {.preset = "custom", .name = "Test", .baseUrl = fake.url(), .key = key, .keyFile = {}, .model = {}};
}

AiRequest asking(const QString &question, AiRequest::Tier tier = AiRequest::Main) {
    return {.system = "Be brief.", .messages = {{"user", question}}, .tier = tier, .model = {}, .prose = false};
}

// Asks and waits for the end: the whole answer, or why it failed.
struct Outcome {
    QString answer, failure;
    QStringList notes;
};

Outcome ask(AiProvider &provider, const AiRequest &request) {
    Outcome outcome;
    bool over = false;
    const QMetaObject::Connection pieces =
        QObject::connect(&provider, &AiProvider::delta, &provider, [&](const QString &text) { outcome.answer += text; });
    const QMetaObject::Connection notes =
        QObject::connect(&provider, &AiProvider::noted, &provider, [&](const QString &note) { outcome.notes << note; });
    const QMetaObject::Connection end = QObject::connect(&provider, &AiProvider::finished, &provider, [&] { over = true; });
    const QMetaObject::Connection failing = QObject::connect(&provider, &AiProvider::failed, &provider, [&](const QString &why) {
        outcome.failure = why;
        over = true;
    });
    provider.ask(request);
    const bool ended = QTest::qWaitFor([&] { return over; }, 10000);
    for (const auto &connection : {pieces, notes, end, failing})
        QObject::disconnect(connection);
    if (!ended)
        outcome.failure = "no answer in time";
    return outcome;
}

// A stand-in for Omarchy's command: what `status --json` and `network --json` print.
QString standInOmarchy(const QString &folder, const QByteArray &status, const QByteArray &network) {
    Fixtures::write(folder + "/status.json", status);
    Fixtures::write(folder + "/network.json", network);
    return script(folder + "/omarchy-agent-local", "case \"$1 $2\" in\n"
                                                   "  'status --json') cat '"
                                                       + folder.toUtf8()
                                                       + "/status.json';;\n"
                                                         "  'network --json') cat '"
                                                       + folder.toUtf8()
                                                       + "/network.json';;\n"
                                                         "  *) exit 2;;\n"
                                                         "esac\n");
}

QByteArray localStatus(const QString &url) {
    return QJsonDocument(
               QJsonObject{
                   {"models",
                    QJsonObject{
                        {"gemma-small",
                         QJsonObject{{"recipe", "gemma-small"}, {"name", "Gemma, small"}, {"unit", "active"}, {"endpoint", url}}},
                        {"qwen-big",
                         QJsonObject{
                             {"recipe", "qwen-big"}, {"name", "Qwen"}, {"unit", "inactive"}, {"endpoint", "http://127.0.0.1:1/v1"}}},
                    }}})
        .toJson();
}

} // namespace

// Servers that speak the OpenAI-compatible API, and Omarchy's local models,
// each against a stand-in: never the network.
class AiServersTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { Secrets::useInstead(&m_secrets); }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    void aServerAnswersAsItWrites() {
        FakeServer fake;
        fake.answer({"Call me ", "Ishmael."});
        OpenAiProvider provider(serverAt(fake, "sk-test"));
        provider.setModels({"big", "small"});
        AiRequest request = asking("His name?");
        request.messages = {{"user", "Who narrates?"}, {"assistant", "A sailor."}, {"user", "His name?"}};
        const Outcome outcome = ask(provider, request);
        QCOMPARE(outcome.failure, QString());
        // Only the answer: the thinking that came with it is never shown.
        QCOMPARE(outcome.answer, QString("Call me Ishmael."));

        QCOMPARE(fake.received.size(), 1);
        const FakeServer::Request &sent = fake.received.first();
        QVERIFY(sent.line.startsWith("POST /v1/chat/completions"));
        QVERIFY(sent.headers.contains("authorization: bearer sk-test"));
        QCOMPARE(sent.body.value("model").toString(), QString("big"));
        QCOMPARE(sent.body.value("stream").toBool(), true);
        // The instructions go first, as the system's message, then the conversation in order.
        const QJsonArray messages = sent.body.value("messages").toArray();
        QCOMPARE(messages.size(), 4);
        QCOMPARE(messages.at(0).toObject().value("role").toString(), QString("system"));
        QCOMPARE(messages.at(0).toObject().value("content").toString(), QString("Be brief."));
        QCOMPARE(messages.at(2).toObject().value("role").toString(), QString("assistant"));
        QCOMPARE(messages.at(3).toObject().value("content").toString(), QString("His name?"));

        // Quick work goes to the quick model.
        fake.answer({"ok"});
        QCOMPARE(ask(provider, asking("q", AiRequest::Quick)).answer, QString("ok"));
        QCOMPARE(fake.received.last().body.value("model").toString(), QString("small"));
    }

    void thinkingIsNeverShown_data() {
        QTest::addColumn<QString>("answer");
        QTest::addColumn<QString>("shown");
        QTest::newRow("at the start") << "<think>secret plans</think>\n\nThe answer." << "The answer.";
        QTest::newRow("the longer tag") << "<thinking>secret</thinking>Done." << "Done.";
        QTest::newRow("no tag") << "No tag at all: a < b, and <b>bold</b>." << "No tag at all: a < b, and <b>bold</b>.";
        QTest::newRow("only thinking") << "<think>secret plans</think>" << "";
        QTest::newRow("never closed") << "<think>secret plans, cut off" << "";
        QTest::newRow("text before and after") << "Before <think>secret</think> after" << "Before  after";
        QTest::newRow("a tag's start that is text") << "1 <thi" << "1 <thi";
        QTest::newRow("a close inside") << "<think>secret </thin secret</think>ok" << "ok";
    }

    void thinkingIsNeverShown() {
        QFETCH(QString, answer);
        QFETCH(QString, shown);
        // However the answer is cut into pieces: whole, in two at every
        // place, in three at every pair of places, and a character at a time.
        // What is passed on only ever grows, so a piece of thinking shown
        // even for a moment would be in what is shown at the end.
        auto through = [](const QStringList &pieces) {
            ThinkingFilter filter;
            QString out;
            for (const QString &piece : pieces)
                out += filter.take(piece);
            return out + filter.finish();
        };
        QCOMPARE(through({answer}), shown);
        for (qsizetype i = 0; i <= answer.size(); ++i) {
            QCOMPARE(through({answer.left(i), answer.mid(i)}), shown);
            for (qsizetype j = i; j <= answer.size(); ++j)
                QCOMPARE(through({answer.left(i), answer.mid(i, j - i), answer.mid(j)}), shown);
        }
        QStringList characters;
        for (const QChar character : std::as_const(answer))
            characters << QString(character);
        QCOMPARE(through(characters), shown);
    }

    void markupAroundAnAnswerIsTakenOut_data() {
        QTest::addColumn<QString>("answer");
        QTest::addColumn<QString>("shown");
        QTest::newRow("wrapped") << "<panel>Edda Marlowe came to keep the harbour light.</panel>"
                                 << "Edda Marlowe came to keep the harbour light.";
        QTest::newRow("nested, on lines of their own") << "<div class=\"answer\">\n  <p>Edda keeps the light.</p>\n</div>\n"
                                                       << "Edda keeps the light.\n";
        QTest::newRow("stray tags, known or not") << "Edda <span>keeps</span> the <x-glow/>light.</p>" << "Edda keeps the light.";
        QTest::newRow("a break") << "One<br>two<br/>\nthree" << "One\ntwo\nthree";
        QTest::newRow("markdown kept")
            << "**Edda** keeps the *light*:\n\n- one\n- two\n\n> said\n\n<https://example.com>, a < b, <- and 1<2."
            << "**Edda** keeps the *light*:\n\n- one\n- two\n\n> said\n\n<https://example.com>, a < b, <- and 1<2.";
        QTest::newRow("indented markdown kept") << "<div>\n- a\n    - b\n</div>" << "- a\n    - b\n";
        QTest::newRow("a code span kept") << "<p>Write `<b>bold</b>` or ``a `<i>` b``.</p>"
                                          << "Write `<b>bold</b>` or ``a `<i>` b``.";
        QTest::newRow("a code block kept")
            << "<div>Like this:\n\n```html\n<panel>\n  <b>x</b>\n</panel>\n```\n\n~~~\n<i>\n~~~\n<b>done</b></div>"
            << "Like this:\n\n```html\n<panel>\n  <b>x</b>\n</panel>\n```\n\n~~~\n<i>\n~~~\ndone";
        QTest::newRow("a code block in a list kept") << "1. Run:\n   ```\n   echo <x>\n   ```\n<p>ok</p>"
                                                     << "1. Run:\n   ```\n   echo <x>\n   ```\nok";
        QTest::newRow("a tag never closed is text") << "Edda <div class" << "Edda <div class";
        QTest::newRow("only markup") << "<div></div>" << "";
    }

    void markupAroundAnAnswerIsTakenOut() {
        QFETCH(QString, answer);
        QFETCH(QString, shown);
        // However the answer is cut: whole, in two at every place, in three
        // at every pair of places, and a character at a time.
        auto through = [](const QStringList &pieces) {
            MarkupFilter filter;
            QString out;
            for (const QString &piece : pieces)
                out += filter.take(piece);
            return out + filter.finish();
        };
        QCOMPARE(through({answer}), shown);
        for (qsizetype i = 0; i <= answer.size(); ++i) {
            QCOMPARE(through({answer.left(i), answer.mid(i)}), shown);
            for (qsizetype j = i; j <= answer.size(); ++j)
                QCOMPARE(through({answer.left(i), answer.mid(i, j - i), answer.mid(j)}), shown);
        }
        QStringList characters;
        for (const QChar character : std::as_const(answer))
            characters << QString(character);
        QCOMPARE(through(characters), shown);
    }

    void anAnswerToReadComesWithoutItsMarkup() {
        // As gemma did through Ollama: the answer wrapped, a tag split between chunks.
        FakeServer fake;
        fake.answer({"<pan", "el>Edda Marlowe", " came.</pa", "nel>"});
        fake.answer({"{\\\"say\\\": \\\"<b>x</b>\\\"}"});
        auto provider = std::make_unique<OpenAiProvider>(serverAt(fake));
        provider->setModels({"gemma", "gemma"});
        AiCall call(std::move(provider));
        QStringList pieces;
        connect(&call, &AiCall::delta, this, [&pieces](const QString &text) { pieces << text; });
        QSignalSpy done(&call, &AiCall::done);
        AiRequest request = asking("Who came?");
        request.prose = true;
        call.ask(request);
        QVERIFY(done.wait(10000));
        QCOMPARE(done.last().first().toString(), QString("Edda Marlowe came."));
        QCOMPARE(pieces.join(QString()), QString("Edda Marlowe came."));
        // What is read as data, not by a person, comes as it was written.
        call.ask(asking("As JSON."));
        QVERIFY(done.wait(10000));
        QCOMPARE(done.last().first().toString(), QString(R"({"say": "<b>x</b>"})"));
    }

    void aServerThatThinksInItsAnswerShowsOnlyTheAnswer() {
        FakeServer fake;
        // As qwen and deepseek do through llama.cpp: the tag split between chunks.
        fake.answer({"<thi", "nk>Who could it be", "? Ishmael.</th", "ink>\\n\\nIshmael", " narrates."});
        OpenAiProvider provider(serverAt(fake));
        provider.setModels({"qwen", "qwen"});
        QStringList pieces;
        connect(&provider, &AiProvider::delta, this, [&pieces](const QString &text) { pieces << text; });
        const Outcome outcome = ask(provider, asking("q"));
        QCOMPARE(outcome.failure, QString());
        QCOMPARE(outcome.answer, QString("Ishmael narrates."));
        for (const QString &piece : std::as_const(pieces))
            QVERIFY2(!piece.contains("Who") && !piece.contains("<"), qPrintable(piece));
    }

    void withoutAKeyNoneIsSent() {
        FakeServer fake;
        fake.answer({"fine"});
        OpenAiProvider provider(serverAt(fake));
        provider.setModels({"gemma", "gemma"});
        QCOMPARE(ask(provider, asking("q")).answer, QString("fine"));
        QVERIFY(!fake.received.first().headers.contains("authorization"));
    }

    void aRefusedModelIsAskedOfTheMainOne() {
        FakeServer fake;
        // As Ollama answers for a model it hasn't pulled.
        fake.refuse("404 Not Found",
                    "{\"error\":{\"message\":\"model \\\"tiny\\\" not found, try pulling it first\",\"type\":\"api_error\"}}");
        fake.answer({"from the main one"});
        OpenAiProvider provider(serverAt(fake));
        provider.setModels({"big", "tiny"});
        const Outcome outcome = ask(provider, asking("q", AiRequest::Quick));
        QCOMPARE(outcome.failure, QString());
        QCOMPARE(outcome.answer, QString("from the main one"));
        QCOMPARE(fake.received.size(), 2);
        QCOMPARE(fake.received.at(0).body.value("model").toString(), QString("tiny"));
        QCOMPARE(fake.received.at(1).body.value("model").toString(), QString("big"));
        QCOMPARE(outcome.notes.size(), 1);
        QVERIFY2(outcome.notes.first().contains("\"tiny\"") && outcome.notes.first().contains("\"big\""),
                 qPrintable(outcome.notes.first()));

        // OpenAI says model_not_found; the main one refused is asked of the
        // server's default, the first model it lists, and that is the end.
        fake.refuse("404 Not Found", R"({"error":{"message":"The model `big` does not exist","code":"model_not_found"}})");
        fake.list({"first"});
        fake.refuse("404 Not Found", R"({"error":{"message":"The model `first` does not exist","code":"model_not_found"}})");
        const Outcome refused = ask(provider, asking("q"));
        QVERIFY2(refused.failure.contains("has no model \"first\""), qPrintable(refused.failure));
        QCOMPARE(fake.received.size(), 5);
        QVERIFY(fake.received.at(3).line.startsWith("GET /v1/models"));
    }

    void noModelChosenIsTheFirstThatRunsOnTheServer() {
        FakeServer fake;
        // Ollama lists its cloud models among its own: those send the text on.
        fake.list({"deepseek-v4-flash:cloud", "gpt-oss:120b-cloud", "gemma4:e2b", "qwen3.8:27b"});
        fake.answer({"hello"});
        OpenAiProvider provider(serverAt(fake, "k"));
        QCOMPARE(ask(provider, asking("q")).answer, QString("hello"));
        QVERIFY(fake.received.at(0).line.startsWith("GET /v1/models"));
        QVERIFY(fake.received.at(0).headers.contains("authorization: bearer k"));
        QCOMPARE(fake.received.at(1).body.value("model").toString(), QString("gemma4:e2b"));
        // Found once, for as long as the provider lives.
        fake.answer({"again"});
        QCOMPARE(ask(provider, asking("q")).answer, QString("again"));
        QCOMPARE(fake.received.size(), 3);

        // Only models that run elsewhere: none is picked for the user.
        FakeServer cloudy;
        cloudy.list({"deepseek-v4-flash:cloud"});
        OpenAiProvider elsewhere(serverAt(cloudy));
        const QString failure = ask(elsewhere, asking("q")).failure;
        QVERIFY2(failure.contains("runs elsewhere"), qPrintable(failure));
        QCOMPARE(cloudy.received.size(), 1);
    }

    void theServersModelsAreListed() {
        FakeServer fake;
        fake.list({"gemma4:e2b", "deepseek-v4-flash:cloud"});
        const auto listed = OpenAiProvider::listModels(serverAt(fake));
        QVERIFY2(listed, qPrintable(Fixtures::whyNot(listed)));
        QCOMPARE(*listed, QStringList({"gemma4:e2b", "deepseek-v4-flash:cloud"}));
        QVERIFY(!fake.received.first().headers.contains("authorization"));
        QVERIFY(AiEndpoints::runsElsewhere("deepseek-v4-flash:cloud"));
        QVERIFY(AiEndpoints::runsElsewhere("gpt-oss:120b-cloud"));
        QVERIFY(!AiEndpoints::runsElsewhere("gemma4:e2b"));

        fake.refuse("401 Unauthorized", R"({"error":{"message":"Incorrect API key provided","code":"invalid_api_key"}})");
        const auto refused = OpenAiProvider::listModels(serverAt(fake, "wrong"));
        QVERIFY(!refused);
        QVERIFY2(Fixtures::whyNot(refused).contains("didn't accept the key"), qPrintable(Fixtures::whyNot(refused)));
    }

    void aServerThatFailsSaysWhy() {
        // Nothing listening: the request fails, as there is nothing else it was asked to use.
        quint16 port = 0;
        {
            QTcpServer closed;
            QVERIFY(closed.listen(QHostAddress::LocalHost));
            port = closed.serverPort();
        }
        OpenAiProvider gone({.preset = "ollama",
                             .name = "Ollama",
                             .baseUrl = QString("http://127.0.0.1:%1/v1").arg(port),
                             .key = {},
                             .keyFile = {},
                             .model = {}});
        gone.setModels({"gemma", "gemma"});
        const QString unreachable = ask(gone, asking("q")).failure;
        QVERIFY2(unreachable.startsWith(QString("Can't reach the AI server at 127.0.0.1:%1").arg(port)), qPrintable(unreachable));

        // A key refused is not a model refused: no other model is tried.
        FakeServer fake;
        fake.refuse("401 Unauthorized", R"({"error":{"message":"Incorrect API key provided"}})");
        OpenAiProvider provider(serverAt(fake, "wrong"));
        provider.setModels({"big", "small"});
        const QString wrongKey = ask(provider, asking("q", AiRequest::Quick)).failure;
        QVERIFY2(wrongKey.contains("didn't accept the key"), qPrintable(wrongKey));
        QCOMPARE(fake.received.size(), 1);

        // An error the server sends in the middle of its stream.
        fake.answer({});
        fake.replies.last().body = "data: {\"error\":{\"message\":\"the server is overloaded\"}}\n\n";
        QVERIFY(ask(provider, asking("q")).failure.contains("the server is overloaded"));
    }

    void omarchysLocalModelsAreFound() {
        QTemporaryDir dir;
        FakeServer fake;
        const QByteArray network =
            QJsonDocument(QJsonObject{{"models",
                                       QJsonObject{
                                           {"desk/qwen-big", QJsonObject{{"host", "desk"},
                                                                         {"recipe", "qwen-big"},
                                                                         {"name", "Qwen"},
                                                                         {"paired", true},
                                                                         {"offline", ""},
                                                                         {"unit", "active"},
                                                                         {"endpoint", "https://desk.example.ts.net/omarchy/qwen-big/v1"}}},
                                           {"far/other", QJsonObject{{"host", "far"},
                                                                     {"recipe", "other"},
                                                                     {"paired", false},
                                                                     {"offline", ""},
                                                                     {"unit", "active"},
                                                                     {"endpoint", "https://far/v1"}}},
                                       }}})
                .toJson();
        const QString omarchy = standInOmarchy(dir.path(), localStatus(fake.url()), network);

        // This computer's running one, and not the one that is stopped.
        auto running = OmarchyLocal::running(omarchy);
        QVERIFY2(running, qPrintable(Fixtures::whyNot(running)));
        QCOMPARE(running->size(), 1);
        QCOMPARE(running->first().model, QString("gemma-small"));
        QCOMPARE(running->first().baseUrl, fake.url());
        QCOMPARE(running->first().keyFile, QDir::homePath() + "/.config/omarchy/agent-local/key");

        // None running here: the ones paired from the tailnet that are up.
        Fixtures::write(dir.filePath("status.json"), R"({"models": {}})");
        running = OmarchyLocal::running(omarchy);
        QVERIFY2(running, qPrintable(Fixtures::whyNot(running)));
        QCOMPARE(running->size(), 1);
        QCOMPARE(running->first().id, QString("desk/qwen-big"));
        QCOMPARE(running->first().model, QString("qwen-big"));
        QCOMPARE(running->first().host, QString("desk"));
        QVERIFY(running->first().keyFile.endsWith("/agent-local/remotes/desk.key"));

        Fixtures::write(dir.filePath("network.json"), R"({"models": {}})");
        running = OmarchyLocal::running(omarchy);
        QVERIFY2(running && running->isEmpty(), qPrintable(Fixtures::whyNot(running)));
        QVERIFY(Fixtures::whyNot(OmarchyLocal::pick(*running, {})).contains("omarchy agent local use"));
        QVERIFY(!OmarchyLocal::running(dir.filePath("missing")));
    }

    void omarchysKeyIsReadForEveryRequest() {
        QTemporaryDir dir;
        FakeServer fake;
        const QString omarchy = standInOmarchy(dir.path(), localStatus(fake.url()), "{}");
        Settings settings(dir.filePath("config.json"));
        settings.setAiProvider("openai");
        settings.setAiEndpoint({{"preset", "omarchy-local"}});
        const auto server = AiEndpoints::server(settings, {}, omarchy);
        QVERIFY2(server, qPrintable(Fixtures::whyNot(server)));
        QCOMPARE(server->name, QString("Omarchy's local model"));
        QCOMPARE(server->model, QString("gemma-small"));

        AiServer here = *server;
        here.keyFile = dir.filePath("key");
        QVERIFY(Fixtures::write(here.keyFile, "first-key\n"));
        OpenAiProvider provider(here);
        fake.answer({"one"});
        QCOMPARE(ask(provider, asking("q")).answer, QString("one"));
        // The model Omarchy runs, asked without listing first.
        QCOMPARE(fake.received.size(), 1);
        QCOMPARE(fake.received.first().body.value("model").toString(), QString("gemma-small"));
        QVERIFY(fake.received.first().headers.contains("authorization: bearer first-key"));
        // Omarchy made a new key meanwhile.
        QVERIFY(Fixtures::write(here.keyFile, "second-key\n"));
        fake.answer({"two"});
        QCOMPARE(ask(provider, asking("q")).answer, QString("two"));
        QVERIFY(fake.received.last().headers.contains("authorization: bearer second-key"));
    }

    void aServerIsUsedOnlyWhenChosen() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const QString noOmarchy = dir.filePath("no-omarchy/agent");
        const QByteArray openAiKey = qgetenv("OPENAI_API_KEY");
        qunsetenv("OPENAI_API_KEY");
        const auto restore = qScopeGuard([&] { openAiKey.isEmpty() ? qunsetenv("OPENAI_API_KEY") : qputenv("OPENAI_API_KEY", openAiKey); });

        // Set up, but not chosen: automatic never switches to it.
        settings.setAiEndpoint({{"preset", "ollama"}});
        settings.setAiProvider("auto");
        QVERIFY(Ai::choose(settings, noOmarchy).id != "openai");

        // Ollama on this computer is the preset with nothing typed in.
        settings.setAiProvider("openai");
        settings.setAiEndpoint({});
        AiChoice choice = Ai::choose(settings, noOmarchy);
        QCOMPARE(choice.id, QString("openai"));
        QCOMPARE(choice.source, AiChoice::Setting);
        QCOMPARE(choice.name, QString("Ollama"));
        QCOMPARE(choice.modelsKey(), QString("openai:http://localhost:11434/v1"));

        // OpenAI needs a key, from its variable or the keyring.
        settings.setAiEndpoint({{"preset", "openai"}});
        choice = Ai::choose(settings, noOmarchy);
        QVERIFY(!choice.available());
        QVERIFY2(choice.problem.contains("OPENAI_API_KEY"), qPrintable(choice.problem));
        qputenv("OPENAI_API_KEY", "sk-from-the-environment");
        QVERIFY(Ai::choose(settings, noOmarchy).available());
        QCOMPARE(AiEndpoints::server(settings)->key, QString("sk-from-the-environment"));
        qunsetenv("OPENAI_API_KEY");
        QVERIFY(AiEndpoints::keepKey("https://api.openai.com/v1/", "sk-kept", m_secrets));
        QCOMPARE(AiEndpoints::server(settings)->key, QString("sk-kept"));

        // Any other by its address; a key is kept by the address, never in config.json.
        settings.setAiEndpoint({{"preset", "custom"}});
        QVERIFY(Ai::choose(settings, noOmarchy).problem.contains("No address"));
        settings.setAiEndpoint({{"preset", "custom"}, {"base_url", "https://api.groq.com/openai/v1/"}});
        choice = Ai::choose(settings, noOmarchy);
        QCOMPARE(choice.name, QString("api.groq.com"));
        QVERIFY(AiEndpoints::keepKey(AiEndpoints::baseUrl(settings), "gsk-secret", m_secrets));
        const QJsonObject described = Ai::describe(settings, noOmarchy);
        QCOMPARE(described.value("provider").toString(), QString("openai"));
        QCOMPARE(described.value("server").toObject().value("base_url").toString(), QString("https://api.groq.com/openai/v1"));
        QCOMPARE(described.value("server").toObject().value("key").toBool(), true);
        QVERIFY(!QJsonDocument(described).toJson().contains("gsk-secret"));
        QFile config(dir.filePath("config.json"));
        QVERIFY(config.open(QIODevice::ReadOnly));
        QVERIFY(!config.readAll().contains("gsk-secret"));

        // Each server's models are its own.
        settings.setAiModelFor(choice.modelsKey(), "main", "llama-4-scout");
        QCOMPARE(Ai::models(settings, choice.modelsKey()).main, QString("llama-4-scout"));
        QCOMPARE(Ai::models(settings, choice.modelsKey()).quick, QString("llama-4-scout"));
        settings.setAiEndpoint({{"preset", "ollama"}});
        QCOMPARE(Ai::models(settings, Ai::choose(settings, noOmarchy).modelsKey()).main, QString());
    }

    void plainHttpBeyondTheTailnetIsSaidToBe() {
        QVERIFY(!AiEndpoints::inTheClear("http://localhost:11434/v1"));
        QVERIFY(!AiEndpoints::inTheClear("http://127.0.0.1:8080/v1"));
        QVERIFY(!AiEndpoints::inTheClear("http://[::1]:8080/v1"));
        QVERIFY(!AiEndpoints::inTheClear("http://desktop:11434/v1"));
        QVERIFY(!AiEndpoints::inTheClear("http://desktop.tail1234.ts.net:11434/v1"));
        QVERIFY(!AiEndpoints::inTheClear("http://100.101.102.103:11434/v1"));
        QVERIFY(!AiEndpoints::inTheClear("https://api.openai.com/v1"));
        QVERIFY(AiEndpoints::inTheClear("http://192.168.1.20:11434/v1"));
        QVERIFY(AiEndpoints::inTheClear("http://models.example.com/v1"));
    }

    void theWindowListsTheServersModels() {
        QTemporaryDir dir;
        FakeServer fake;
        fake.list({"gemma4:e2b", "deepseek-v4-flash:cloud"});
        Settings settings(dir.filePath("config.json"));
        settings.setAiProvider("openai");
        // Set before the agent is made, which lists the models of the server in use at once.
        settings.setAiEndpoint({{"preset", "custom"}, {"base_url", fake.url()}});
        AiAgent agent(&settings, dir.filePath("no-omarchy/agent"));
        QCOMPARE(agent.serverPreset(), QString("custom"));
        QCOMPARE(agent.serverUrl(), fake.url());
        QVERIFY(!agent.serverInTheClear());
        QVERIFY(QTest::qWaitFor([&] { return !agent.knownModels().isEmpty(); }, 5000));
        QCOMPARE(agent.knownModels(), QStringList({"gemma4:e2b", "deepseek-v4-flash:cloud"}));
        QCOMPARE(agent.elsewhereModels(), QStringList({"deepseek-v4-flash:cloud"}));
        QCOMPARE(agent.modelsProblem(), QString());
        QVERIFY(!agent.serverHasKey());

        // A key: kept in the keyring, and the models asked for again with it.
        fake.list({"gemma4:e2b"});
        QCOMPARE(agent.setServerKey("sk-window"), QString());
        QVERIFY(agent.serverHasKey());
        QVERIFY(QTest::qWaitFor([&] { return agent.knownModels().size() == 1; }, 5000));
        QVERIFY(fake.received.last().headers.contains("authorization: bearer sk-window"));
        QCOMPARE(m_secrets.get("ai-server:" + fake.url()), QString("sk-window"));

        // A model picked is kept for this server.
        agent.setModel("main", "gemma4:e2b");
        QCOMPARE(settings.aiModelFor("openai:" + fake.url(), "main"), QString("gemma4:e2b"));
        QCOMPARE(agent.mainModel(), QString("gemma4:e2b"));
        QCOMPARE(agent.setServerKey(""), QString());
        QVERIFY(!agent.serverHasKey());

        // The preset's own address is not kept: it follows the preset. (With
        // the AI off, so nothing asks a server on this computer.)
        settings.setAiProvider("off");
        agent.setServer("ollama", "http://localhost:11434/v1");
        QCOMPARE(settings.aiEndpoint().value("preset").toString(), QString("ollama"));
        QVERIFY(!settings.aiEndpoint().contains("base_url"));
    }

    void theCommandLineAsksTheServer() {
        QTemporaryDir dir;
        FakeServer fake;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const char *folder : {"config", "data", "cache", "state"})
            QVERIFY(QDir().mkpath(dir.filePath(folder)));
        environment.insert("HOME", dir.path());
        environment.insert("XDG_CONFIG_HOME", dir.filePath("config"));
        environment.insert("XDG_DATA_HOME", dir.filePath("data"));
        environment.insert("XDG_CACHE_HOME", dir.filePath("cache"));
        environment.insert("XDG_STATE_HOME", dir.filePath("state"));
        // No keyring to reach: a key is then kept for the session only, and
        // the test's home holds no Omarchy default agent.
        environment.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + dir.filePath("no-bus"));
        // The server answers in this event loop, so the command is waited
        // for without blocking it.
        auto run = [&](const QStringList &arguments) {
            QProcess omabook;
            omabook.setProcessEnvironment(environment);
            omabook.start(OMABOOK_BINARY, arguments);
            omabook.closeWriteChannel();
            const bool ended = QTest::qWaitFor([&] { return omabook.state() == QProcess::NotRunning; }, 20000);
            return std::make_pair(ended && omabook.exitCode() == 0,
                                  QString::fromUtf8(omabook.readAllStandardOutput() + omabook.readAllStandardError()));
        };
        auto [used, said] = run({"ai", "use", "custom", fake.url()});
        QVERIFY2(used, qPrintable(said));
        fake.list({"gemma4:e2b", "deepseek-v4-flash:cloud"});
        auto [listed, models] = run({"ai", "models"});
        QVERIFY2(listed, qPrintable(models));
        QVERIFY2(models.contains("gemma4:e2b\n") && models.contains("deepseek-v4-flash:cloud\t(runs elsewhere"), qPrintable(models));
        auto [shown, info] = run({"info", "--json"});
        QVERIFY2(shown, qPrintable(info));
        const QJsonObject ai = QJsonDocument::fromJson(info.toUtf8()).object().value("ai").toObject();
        QCOMPARE(ai.value("provider").toString(), QString("openai"));
        QCOMPARE(ai.value("server").toObject().value("preset").toString(), QString("custom"));
        QCOMPARE(ai.value("server").toObject().value("key").toBool(), false);
    }

private:
    SessionSecrets m_secrets;
};

QTEST_MAIN(AiServersTests)
#include "aiservers_tests.moc"
