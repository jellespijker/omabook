#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QtTest>
#include <unistd.h>

#include "ai.h"
#include "bookops.h"
#include "converter.h"
#include "curation.h"
#include "customization.h"
#include "fixtures.h"
#include "interfacechange.h"
#include "metadatasource.h"
#include "plugins.h"
#include "blurb.h"
#include "prompts.h"
#include "library.h"
#include "settings.h"
#include "sync.h"

namespace {

using Fixtures::script;

QString standInSsh(const QTemporaryDir &dir) { return Fixtures::standInSsh(dir.path(), OMABOOK_BINARY); }

// This computer's home, and so its trash, in a test's folder for as long as
// it lives.
class HomeIn {
public:
    explicit HomeIn(const QString &path) : m_home(qgetenv("HOME")), m_data(qgetenv("XDG_DATA_HOME")) {
        QDir().mkpath(path + "/.local/share/Trash/files");
        QDir().mkpath(path + "/.local/share/Trash/info");
        qputenv("HOME", path.toUtf8());
        qputenv("XDG_DATA_HOME", (path + "/.local/share").toUtf8());
    }
    ~HomeIn() {
        qputenv("HOME", m_home);
        m_data.isEmpty() ? qunsetenv("XDG_DATA_HOME") : qputenv("XDG_DATA_HOME", m_data);
    }
    HomeIn(const HomeIn &) = delete;
    HomeIn &operator=(const HomeIn &) = delete;

private:
    QByteArray m_home, m_data;
};

// Runs a statement on a library's database as it is: for the times a test
// must set, which nothing in Omabook sets by hand.
bool direct(const Library &library, const QString &sql) {
    const QString name = "direct-" + QUuid::createUuid().toString(QUuid::Id128);
    bool done = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", name);
        database.setDatabaseName(library.databasePath());
        database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=10000");
        done = database.open() && QSqlQuery(database).exec(sql);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return done;
}

// What a library holds, to tell that nothing changed.
QByteArray snapshot(const Library &library) { return QJsonDocument(libraryManifest(library)).toJson(); }

// Where what was under `folder` went to the trash, as each trash entry
// says it was ("Path=" in its .trashinfo): in the trash of one of `homes`,
// or the one at the top of the folder's disk, as the system chose.
QStringList trashedFrom(const QString &folder, const QStringList &homes) {
    QStringList trashes;
    for (const QString &home : homes)
        trashes << home + "/.local/share/Trash";
    const QString top = QStorageInfo(folder).rootPath();
    trashes << QString("%1/.Trash-%2").arg(top).arg(getuid()) << QString("%1/.Trash/%2").arg(top).arg(getuid());
    // A trash at the top of a disk keeps the path from that top.
    const QStringList from{folder + "/", QDir(top).relativeFilePath(folder) + "/"};
    QStringList found;
    for (const QString &trash : trashes)
        for (const QFileInfo &info : QDir(trash + "/info").entryInfoList({"*.trashinfo"}, QDir::Files)) {
            QFile file(info.filePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            for (const QString &line : QString::fromUtf8(file.readAll()).split('\n')) {
                const QString path = QUrl::fromPercentEncoding(line.mid(5).toUtf8());
                for (const QString &start : from)
                    if (line.startsWith("Path=") && path.startsWith(start))
                        found << path.mid(start.size());
            }
        }
    return found;
}

// An HTTP server that answers every request with one canned response and
// keeps what it was sent, standing in for the Anthropic API.
class FakeApi : public QTcpServer {
public:
    QByteArray status = "200 OK";
    QByteArray body;
    QByteArray received;

    FakeApi() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                received += socket->readAll();
                const qsizetype headerEnd = received.indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                const QByteArray lower = received.left(headerEnd).toLower();
                const qsizetype at = lower.indexOf("content-length:");
                const qsizetype length = at < 0 ? 0 : lower.mid(at + 15, lower.indexOf("\r\n", at) - at - 15).trimmed().toLongLong();
                if (received.size() < headerEnd + 4 + length)
                    return;
                socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/event-stream\r\nContent-Length: "
                              + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }

    QString url() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
    QJsonObject request() const { return QJsonDocument::fromJson(received.mid(received.indexOf("\r\n\r\n") + 4)).object(); }
};

QByteArray sse(const QByteArray &json) { return "event: x\ndata: " + json + "\n\n"; }

// What a test asks: the instructions and what was said, with the tier and
// the model left as a request has them when nothing is said of either.
AiRequest asking(const QString &system, const QList<AiMessage> &messages) {
    return {.system = system, .messages = messages, .tier = AiRequest::Main, .model = {}, .prose = false};
}

} // namespace

// The services at the library's edges: AI, catalogs, conversion. Each is
// tested against a stand-in, never the network or the user's own tools.
class ServicesTests : public QObject {
    Q_OBJECT

private slots:
    void claudeAnswersInPieces() {
        FakeApi api;
        api.body = sse("{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"}}")
                   + sse("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hm\"}}")
                   + sse("{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Call me \"}}")
                   + sse("{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Ishmael.\"}}")
                   + sse("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}") + sse("{\"type\":\"message_stop\"}");
        AnthropicProvider provider("test-key", {}, api.url());
        QString answer;
        connect(&provider, &AiProvider::delta, this, [&](const QString &text) { answer += text; });
        QSignalSpy finished(&provider, &AiProvider::finished), failed(&provider, &AiProvider::failed);

        AiRequest request;
        request.system = "Be brief.";
        request.messages = {{"user", "Who narrates?"}, {"assistant", "A sailor."}, {"user", "His name?"}};
        provider.ask(request);
        QVERIFY(finished.wait(5000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(answer, QString("Call me Ishmael."));

        // What was sent: the key, the version, a streamed request for the
        // default model with the conversation in order.
        const QByteArray headers = api.received.left(api.received.indexOf("\r\n\r\n")).toLower();
        QVERIFY(headers.startsWith("post /v1/messages"));
        QVERIFY(headers.contains("x-api-key: test-key"));
        QVERIFY(headers.contains("anthropic-version: 2026-10-21"));
        const QJsonObject sent = api.request();
        QCOMPARE(sent.value("model").toString(), QString("claude-opus-5-5"));
        QCOMPARE(sent.value("stream").toBool(), true);
        QCOMPARE(sent.value("system").toString(), QString("Be brief."));
        QCOMPARE(sent.value("messages").toArray().size(), 3);
        QCOMPARE(sent.value("messages").toArray().at(2).toObject().value("content").toString(), QString("His name?"));
        QVERIFY(!sent.contains("temperature"));
    }

    void claudeSaysWhyItFailed() {
        FakeApi api;
        api.status = "401 Unauthorized";
        api.body = "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}";
        AnthropicProvider provider("bad", "claude-haiku-5-5", api.url());
        QSignalSpy failed(&provider, &AiProvider::failed);
        provider.ask(asking("s", {{"user", "q"}}));
        QVERIFY(failed.wait(5000));
        QVERIFY2(failed.first().first().toString().contains("ANTHROPIC_API_KEY"), qPrintable(failed.first().first().toString()));
        // A model that has no server-side fallback isn't sent the option.
        QVERIFY(!api.request().contains("fallbacks"));
        QCOMPARE(api.request().value("model").toString(), QString("claude-haiku-5-5"));
    }

    void aDeclinedOrBrokenAnswerIsAnError() {
        FakeApi declined;
        declined.body = sse("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\"}}") + sse("{\"type\":\"message_stop\"}");
        AnthropicProvider provider("k", {}, declined.url());
        QSignalSpy failed(&provider, &AiProvider::failed);
        provider.ask(asking("s", {{"user", "q"}}));
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.first().first().toString().contains("declined"));

        FakeApi overloaded;
        overloaded.body = sse("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}");
        AnthropicProvider second("k", {}, overloaded.url());
        QSignalSpy secondFailed(&second, &AiProvider::failed);
        second.ask(asking("s", {{"user", "q"}}));
        QVERIFY(secondFailed.wait(5000));
        QVERIFY(secondFailed.first().first().toString().contains("Overloaded"));
    }

    void aCommandIsHandedThePromptAndAnswers() {
        QTemporaryDir dir;
        // Echoes its input, so the answer shows what the command was sent.
        CommandProvider provider({script(dir.filePath("my model"), "echo \"got: $1\"\ncat\n"), "--flag"});
        QCOMPARE(provider.name(), QString("my model"));
        QString answer;
        connect(&provider, &AiProvider::delta, this, [&](const QString &text) { answer += text; });
        QSignalSpy finished(&provider, &AiProvider::finished);
        provider.ask(asking("Instructions here.", {{"user", "First?"}, {"assistant", "Yes."}, {"user", "Second?"}}));
        QVERIFY(finished.wait(5000));
        QVERIFY2(answer.startsWith("got: --flag\nInstructions here."), qPrintable(answer));
        QVERIFY(answer.indexOf("First?") < answer.indexOf("Yes."));
        QVERIFY(answer.indexOf("Yes.") < answer.indexOf("Second?"));
    }

    void aFailingOrMissingCommandSaysSo() {
        QTemporaryDir dir;
        CommandProvider failing({script(dir.filePath("broken"), "echo 'no model loaded' >&2\nexit 3\n")});
        QSignalSpy failed(&failing, &AiProvider::failed);
        failing.ask(asking("s", {{"user", "q"}}));
        QVERIFY(failed.wait(5000));
        QVERIFY2(failed.first().first().toString().contains("no model loaded"), qPrintable(failed.first().first().toString()));

        CommandProvider missing({dir.filePath("not-there")});
        QSignalSpy missingFailed(&missing, &AiProvider::failed);
        missing.ask(asking("s", {{"user", "q"}}));
        QVERIFY(missingFailed.wait(5000));
        QVERIFY(missingFailed.first().first().toString().contains("can't be started"));
    }

    void antigravityAnswersThroughItsOwnProtocol() {
        QTemporaryDir dir;
        // A stand-in agy: it records how it was run and what it was sent, and
        // answers the way the real one does, an event per line.
        const QString agy = script(dir.filePath("agy"),
            "echo \"$@\" > '" + dir.path().toUtf8() + "/arguments'\npwd > '" + dir.path().toUtf8() + "/folder'\n"
            "cat > '" + dir.path().toUtf8() + "/sent'\n"
            "echo '{\"event\":\"init\",\"init\":{}}'\n"
            "echo '{\"event\":\"step_update\",\"step_update\":{\"step_type\":\"planner\",\"text_delta\":\"thinking\"}}'\n"
            "echo '{\"event\":\"step_update\",\"step_update\":{\"step_type\":\"agent_response\",\"text_delta\":\"Call me \"}}'\n"
            "echo '{\"event\":\"step_update\",\"step_update\":{\"step_type\":\"agent_response\",\"text_delta\":\"Ishmael.\"}}'\n"
            "echo '{\"event\":\"result\",\"result\":{\"status\":\"SUCCESS\",\"response\":\"Call me Ishmael.\"}}'\n");
        AntigravityProvider provider(agy);
        QString answer;
        connect(&provider, &AiProvider::delta, this, [&](const QString &text) { answer += text; });
        QSignalSpy finished(&provider, &AiProvider::finished);
        const QString longChapter = QString(300000, 'x'); // longer than an argument may be
        provider.ask(asking("Be brief.", {{"user", longChapter + " Who narrates?"}}));
        QVERIFY(finished.wait(5000));
        QCOMPARE(answer, QString("Call me Ishmael."));

        QFile arguments(dir.filePath("arguments")), folder(dir.filePath("folder")), sent(dir.filePath("sent"));
        QVERIFY(arguments.open(QIODevice::ReadOnly) && folder.open(QIODevice::ReadOnly) && sent.open(QIODevice::ReadOnly));
        // Plan mode and a sandbox, in a folder of its own with nothing in it.
        const QString ran = QString::fromUtf8(arguments.readAll());
        QVERIFY2(ran.contains("--mode plan") && ran.contains("--sandbox"), qPrintable(ran));
        const QString where = QString::fromUtf8(folder.readAll()).trimmed();
        QVERIFY(where != QDir::currentPath());
        QVERIFY(QDir(where).isEmpty());
        // The whole prompt went in on standard input, as one JSON line.
        const QJsonObject turn = QJsonDocument::fromJson(sent.readAll()).object();
        QCOMPARE(turn.value("event").toString(), QString("user"));
        const QString content = turn.value("message").toObject().value("content").toString();
        // Told first that it has no tools (it would reach for them and end
        // with no answer), then the instructions it was given.
        QVERIFY(content.startsWith("You have no tools in this conversation"));
        QVERIFY(content.contains("\n\nBe brief."));
        QVERIFY(content.contains(longChapter));
        QVERIFY(content.trimmed().endsWith("Who narrates?"));
    }

    void antigravitySaysWhyItFailed() {
        QTemporaryDir dir;
        AntigravityProvider failing(script(dir.filePath("agy"),
            "cat > /dev/null\necho '{\"event\":\"result\",\"result\":{\"status\":\"ERROR\",\"error\":\"quota exceeded\"}}'\n"));
        QSignalSpy failed(&failing, &AiProvider::failed);
        failing.ask(asking("s", {{"user", "q"}}));
        QVERIFY(failed.wait(5000));
        QVERIFY2(failed.first().first().toString().contains("quota exceeded"), qPrintable(failed.first().first().toString()));

        AntigravityProvider silent(script(dir.filePath("quiet"), "cat > /dev/null\n"));
        QSignalSpy silentFailed(&silent, &AiProvider::failed);
        silent.ask(asking("s", {{"user", "q"}}));
        QVERIFY(silentFailed.wait(5000));
        QVERIFY(silentFailed.first().first().toString().contains("sign in"));
    }

    void claudeCodeRunsWithEveryToolOff() {
        // The book's text must never be able to make an agent act.
        const QStringList arguments = CommandProvider::claudeCommand("/usr/bin/claude");
        QCOMPARE(arguments.first(), QString("/usr/bin/claude"));
        QVERIFY(arguments.contains("-p"));
        QCOMPARE(arguments.value(arguments.indexOf("--tools") + 1), QString(""));
        QVERIFY(arguments.contains("--strict-mcp-config"));
    }

    void theSettingsChooseTheProvider() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        QString why;

        settings.setAiProvider("off");
        QVERIFY(!makeAiProvider(settings, &why));
        QVERIFY(why.contains("turned off"));

        settings.setAiProvider("command");
        QVERIFY(!makeAiProvider(settings, &why));
        QVERIFY(why.contains("No AI command"));
        settings.setAiCommand("ollama run llama3");
        QCOMPARE(makeAiProvider(settings, &why)->name(), QString("ollama"));

        const QByteArray key = qgetenv("ANTHROPIC_API_KEY");
        qunsetenv("ANTHROPIC_API_KEY");
        settings.setAiProvider("anthropic");
        QVERIFY(!makeAiProvider(settings, &why));
        QVERIFY(why.contains("ANTHROPIC_API_KEY"));
        qputenv("ANTHROPIC_API_KEY", "k");
        // Without Omarchy (no defaults file), a key still comes first.
        settings.setAiProvider("auto");
        QCOMPARE(makeAiProvider(settings, &why, dir.filePath("no-omarchy/agent"))->name(), QString("Claude"));
        if (key.isEmpty()) qunsetenv("ANTHROPIC_API_KEY");
        else qputenv("ANTHROPIC_API_KEY", key);
    }

    void automaticIsOmarchysDefaultAgent() {
        // A machine of the test's own: three harnesses on the PATH, a key in
        // the environment, and Omarchy's default agent in a file.
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const QString defaults = dir.filePath("omarchy/defaults/agent");
        QVERIFY(QDir().mkpath(dir.filePath("omarchy/defaults")) && QDir().mkpath(dir.filePath("bin")));
        for (const char *harness : {"claude", "codex", "agy"})
            script(dir.filePath(QString("bin/") + harness), "cat\n");
        const QByteArray path = qgetenv("PATH"), key = qgetenv("ANTHROPIC_API_KEY");
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", path);
            key.isEmpty() ? qunsetenv("ANTHROPIC_API_KEY") : qputenv("ANTHROPIC_API_KEY", key);
        });
        qputenv("PATH", dir.filePath("bin").toUtf8());
        qputenv("ANTHROPIC_API_KEY", "k");
        QString why;

        // The default agent answers, and the key no longer comes before it.
        QVERIFY(Fixtures::write(defaults, "codex\n"));
        AiChoice choice = Ai::choose(settings, defaults);
        QCOMPARE(choice.source, AiChoice::OmarchyDefault);
        QCOMPARE(choice.id, QString("codex"));
        QCOMPARE(choice.name, QString("Codex"));
        QCOMPARE(choice.program, dir.filePath("bin/codex"));
        QCOMPARE(makeAiProvider(settings, &why, defaults)->name(), QString("Codex"));
        QVERIFY(why.isEmpty());
        QCOMPARE(choice.toJson().value("source").toString(), QString("omarchy"));
        QVERIFY(Fixtures::write(defaults, "agy\n"));
        QCOMPARE(makeAiProvider(settings, &why, defaults)->name(), QString("Antigravity"));

        // What is chosen in Omabook's own settings still wins.
        settings.setAiProvider("claude");
        choice = Ai::choose(settings, defaults);
        QCOMPARE(choice.source, AiChoice::Setting);
        QCOMPARE(choice.id, QString("claude"));
        settings.setAiProvider("anthropic");
        QCOMPARE(Ai::choose(settings, defaults).id, QString("anthropic"));
        settings.setAiProvider("grok");
        QVERIFY(!makeAiProvider(settings, &why, defaults));
        QVERIFY2(why.contains("Grok isn't installed"), qPrintable(why));
        settings.setAiProvider("auto");

        // A default Omabook can't run with its tools off, or one that isn't
        // installed: the old order, and it says which answers instead.
        QVERIFY(Fixtures::write(defaults, "crush\n"));
        choice = Ai::choose(settings, defaults);
        QCOMPARE(choice.source, AiChoice::Fallback);
        QCOMPARE(choice.id, QString("claude"));
        QCOMPARE(choice.omarchyDefault, QString("crush"));
        QVERIFY2(choice.problem.contains("Crush") && choice.problem.contains("Claude Code answers instead"), qPrintable(choice.problem));
        QVERIFY(Fixtures::write(defaults, "grok\n"));
        choice = Ai::choose(settings, defaults);
        QCOMPARE(choice.source, AiChoice::Fallback);
        QVERIFY2(choice.problem.contains("Grok, isn't installed"), qPrintable(choice.problem));
        QVERIFY(QFile::remove(dir.filePath("bin/claude")));
        QCOMPARE(Ai::choose(settings, defaults).id, QString("agy"));

        // With no agent installed at all, the key is what is left.
        QVERIFY(QFile::remove(dir.filePath("bin/agy")) && QFile::remove(dir.filePath("bin/codex")));
        QVERIFY(Fixtures::write(defaults, "codex\n"));
        choice = Ai::choose(settings, defaults);
        QCOMPARE(choice.source, AiChoice::Fallback);
        QCOMPARE(choice.id, QString("anthropic"));
        qunsetenv("ANTHROPIC_API_KEY");
        QVERIFY(!makeAiProvider(settings, &why, defaults));
        QVERIFY2(why.contains("Codex, isn't installed") && why.contains("No AI is set up"), qPrintable(why));

        // Without Omarchy it is as it always was: the key, then Claude Code.
        QVERIFY(QFile::remove(defaults));
        script(dir.filePath("bin/claude"), "cat\n");
        choice = Ai::choose(settings, defaults);
        QCOMPARE(choice.source, AiChoice::Automatic);
        QCOMPARE(choice.id, QString("claude"));
        qputenv("ANTHROPIC_API_KEY", "k");
        QCOMPARE(Ai::choose(settings, defaults).id, QString("anthropic"));
        // A file that names nothing is Omarchy with no default chosen.
        QVERIFY(Fixtures::write(defaults, "\n"));
        QCOMPARE(Ai::choose(settings, defaults).source, AiChoice::Automatic);
    }

    void eachHarnessIsRunForOneAnswerWithItsToolsOff() {
        QTemporaryDir dir;
        const QByteArray here = dir.path().toUtf8();
        AiRequest request;
        request.system = "You are a reading companion.";
        request.messages.append(AiMessage{"user", "Who is Ishmael?"});
        auto answerOf = [&](AiProvider &provider) {
            QString answer;
            const auto pieces = connect(&provider, &AiProvider::delta, this, [&](const QString &text) { answer += text; });
            QSignalSpy finished(&provider, &AiProvider::finished), failed(&provider, &AiProvider::failed);
            provider.ask(request);
            if (!finished.wait(5000))
                answer = QString("failed: ") + (failed.isEmpty() ? QString("no answer") : failed.first().first().toString());
            disconnect(pieces);
            return answer;
        };
        auto recorded = [&](const QString &name) {
            QFile file(dir.filePath(name));
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };

        // Codex: one headless turn that keeps no session, without the user's
        // config (and so their MCP servers), read-only, in an empty folder,
        // with the question on standard input.
        const QString codex = script(dir.filePath("codex"),
            "echo \"$@\" > '" + here + "/arguments'\npwd > '" + here + "/folder'\nls -A | wc -l > '" + here + "/files'\n"
            "echo \"home:$CODEX_HOME\" > '" + here + "/home'\ncat > '" + here + "/sent'\necho 'The narrator.'\n");
        const auto viaCodex = CommandProvider::agent("codex", codex);
        QVERIFY(viaCodex);
        QCOMPARE(viaCodex->name(), QString("Codex"));
        QCOMPARE(answerOf(*viaCodex).trimmed(), QString("The narrator."));
        const QString arguments = recorded("arguments");
        QVERIFY2(arguments.startsWith("exec ") && arguments.trimmed().endsWith(" -"), qPrintable(arguments));
        for (const char *flag : {"--ephemeral", "--ignore-user-config", "--sandbox read-only", "--skip-git-repo-check"})
            QVERIFY2(arguments.contains(flag), flag);
        QVERIFY(recorded("folder").trimmed() != QDir::currentPath());
        QCOMPARE(recorded("files").trimmed(), QString("0"));
        QVERIFY(recorded("sent").contains("You are a reading companion.") && recorded("sent").contains("Who is Ishmael?"));

        // The subscription Omarchy has selected is the one that is asked.
        const QString helper = script(dir.filePath("account-home"), "echo \"/accounts/$1/work\"\n");
        viaCodex->setAccount(helper, "codex", "CODEX_HOME");
        const QByteArray pinned = qgetenv("CODEX_HOME");
        qunsetenv("CODEX_HOME");
        QCOMPARE(answerOf(*viaCodex).trimmed(), QString("The narrator."));
        QCOMPARE(recorded("home").trimmed(), QString("home:/accounts/codex/work"));
        // One the user's own environment names is left alone.
        qputenv("CODEX_HOME", "/mine");
        QCOMPARE(answerOf(*viaCodex).trimmed(), QString("The narrator."));
        QCOMPARE(recorded("home").trimmed(), QString("home:/mine"));
        pinned.isEmpty() ? qunsetenv("CODEX_HOME") : qputenv("CODEX_HOME", pinned);

        // Grok and Pi take the question from a file: it is far longer than
        // an argument may be. Grok plans only; Pi has every tool off.
        const QString grok = script(dir.filePath("grok"),
            "echo \"$@\" > '" + here + "/grok-arguments'\nwhile [ $# -gt 0 ]; do [ \"$1\" = --prompt-file ] && cat \"$2\"; shift; done\n");
        const auto viaGrok = CommandProvider::agent("grok", grok);
        QVERIFY(viaGrok);
        const QString fromGrok = answerOf(*viaGrok);
        QVERIFY2(fromGrok.contains("Who is Ishmael?"), qPrintable(fromGrok));
        QVERIFY(recorded("grok-arguments").contains("--permission-mode plan"));
        QVERIFY(recorded("grok-arguments").contains("--no-subagents"));
        QVERIFY(!recorded("grok-arguments").contains("{prompt-file}"));

        const QString pi = script(dir.filePath("pi"),
            "echo \"$@\" > '" + here + "/pi-arguments'\nfor word in \"$@\"; do case \"$word\" in @*) cat \"${word#@}\";; esac; done\n");
        const auto viaPi = CommandProvider::agent("pi", pi);
        QVERIFY(viaPi);
        const QString fromPi = answerOf(*viaPi);
        QVERIFY2(fromPi.contains("Who is Ishmael?"), qPrintable(fromPi));
        for (const char *flag : {"-p", "--no-session", "--no-tools", "--no-mcp", "--no-extensions"})
            QVERIFY2(recorded("pi-arguments").contains(flag), flag);

        // A harness Omabook has no safe way to run is not run at all.
        QVERIFY(!CommandProvider::agent("crush", "/usr/bin/crush"));
        QVERIFY(!Ai::canRun("opencode"));
        QCOMPARE(Ai::agentName("cursor-agent"), QString("Cursor CLI"));
    }

    void theCompanionFollowsOmarchysDefaultLive() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        QVERIFY(QDir().mkpath(dir.filePath("bin")));
        for (const char *harness : {"claude", "codex"})
            script(dir.filePath(QString("bin/") + harness), "cat\n");
        const QByteArray path = qgetenv("PATH"), key = qgetenv("ANTHROPIC_API_KEY");
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", path);
            if (!key.isEmpty()) qputenv("ANTHROPIC_API_KEY", key);
        });
        qputenv("PATH", dir.filePath("bin").toUtf8());
        qunsetenv("ANTHROPIC_API_KEY");

        // No Omarchy yet: its folder isn't even there.
        const QString defaults = dir.filePath("config/omarchy/defaults/agent");
        AiAgent agent(&settings, defaults);
        QSignalSpy changed(&agent, &AiAgent::changed);
        QCOMPARE(agent.id(), QString("claude"));
        QCOMPARE(agent.source(), QString("automatic"));

        // A default is picked for the first time, in folders that are new.
        QVERIFY(QDir().mkpath(dir.filePath("config/omarchy/defaults")));
        QVERIFY(Fixtures::write(defaults, "codex\n"));
        QTRY_COMPARE(agent.id(), QString("codex"));
        QCOMPARE(agent.source(), QString("omarchy"));
        QCOMPARE(agent.name(), QString("Codex"));
        QVERIFY(!changed.isEmpty());
        QCOMPARE(agent.makeProvider()->name(), QString("Codex"));

        // Switched by replacing the file, as a careful writer does.
        QVERIFY(Fixtures::write(defaults + ".new", "claude\n"));
        QVERIFY(QFile::remove(defaults) && QFile::rename(defaults + ".new", defaults));
        QTRY_COMPARE(agent.id(), QString("claude"));
        QCOMPARE(agent.source(), QString("omarchy"));
        // And again, to know the watch was armed again after the replace.
        QVERIFY(Fixtures::write(defaults, "crush\n"));
        QTRY_COMPARE(agent.source(), QString("fallback"));
        QCOMPARE(agent.omarchyDefaultName(), QString("Crush"));
        QVERIFY(!agent.omarchyDefaultRuns());
        QCOMPARE(agent.id(), QString("claude"));

        // Omabook's own setting is followed too, and wins.
        settings.setAiProvider("off");
        QCOMPARE(agent.source(), QString("none"));
        QVERIFY(!agent.available());
    }

    void eachKindOfWorkHasItsModel() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        // Nothing chosen: the harness's own default for what is read, and its
        // small model, where one is known, for reading and parsing.
        AiModels models = Ai::models(settings, "claude");
        QCOMPARE(models.main, QString());
        QCOMPARE(models.quick, QString("haiku"));
        QCOMPARE(Ai::models(settings, "anthropic").main, QString("claude-opus-5-5"));
        QCOMPARE(Ai::models(settings, "anthropic").quick, QString("claude-haiku-5-5"));
        QCOMPARE(Ai::models(settings, "codex").quick, QString("gpt-6-luna"));
        QCOMPARE(Ai::models(settings, "agy").quick, QString("gemini-3.8-flash-medium"));
        // No small model known: quick work goes to the main one, not to a guess.
        for (const char *harness : {"grok", "pi", "command"}) {
            QCOMPARE(Ai::models(settings, harness).quick, QString());
            QVERIFY(Ai::catalog(harness).quick.isEmpty());
        }
        settings.setAiModelFor("grok", "main", " grok-4.5 ");
        QCOMPARE(Ai::models(settings, "grok").main, QString("grok-4.5"));
        QCOMPARE(Ai::models(settings, "grok").quick, QString("grok-4.5"));

        // A choice is kept per provider, and read back from the file.
        settings.setAiModelFor("claude", "main", "opus");
        settings.setAiModelFor("claude", "quick", "sonnet");
        {
            const Settings again(dir.filePath("config.json"));
            QCOMPARE(Ai::models(again, "claude").main, QString("opus"));
            QCOMPARE(Ai::models(again, "claude").quick, QString("sonnet"));
            QCOMPARE(Ai::models(again, "codex").main, QString());
        }
        settings.setAiModelFor("claude", "quick", "");
        QCOMPARE(Ai::models(settings, "claude").quick, QString("haiku"));

        // The older key is still the Anthropic API's main model, until a newer choice is made.
        QVERIFY(Fixtures::write(dir.filePath("old.json"), "{\"ai_model\": \"claude-sonnet-5-5\"}"));
        Settings old(dir.filePath("old.json"));
        QCOMPARE(Ai::models(old, "anthropic").main, QString("claude-sonnet-5-5"));
        QCOMPARE(Ai::models(old, "anthropic").quick, QString("claude-haiku-5-5"));
        QCOMPARE(Ai::models(old, "claude").main, QString());
        old.setAiModelFor("anthropic", "main", "");
        QCOMPARE(Ai::models(old, "anthropic").main, QString("claude-opus-5-5"));

        // What asks decides the tier; a word or a name can overrule it.
        QCOMPARE(Prompts::graphExtraction({}, {}, "text", {}).tier, AiRequest::Quick);
        QCOMPARE(Prompts::metadata("a.epub", {}, "opening").tier, AiRequest::Quick);
        QCOMPARE(Curation::seriesRequest({}).tier, AiRequest::Quick);
        QCOMPARE(Prompts::change({}, "warmer").tier, AiRequest::Main);
        QCOMPARE(Curation::libraryQuestion({}, "what?").tier, AiRequest::Main);
        QCOMPARE(Curation::suggestionRequest({}).tier, AiRequest::Main);
        CommandProvider provider({"cat"});
        provider.setModels({"big", "small"});
        AiRequest request;
        QCOMPARE(provider.modelFor(request), QString("big"));
        request.tier = AiRequest::Quick;
        QCOMPARE(provider.modelFor(request), QString("small"));
        Ai::useModel(request, "main");
        QCOMPARE(provider.modelFor(request), QString("big"));
        Ai::useModel(request, " other ");
        QCOMPARE(provider.modelFor(request), QString("other"));
        // For one run of a command: every request, unless it names its own.
        AiRequest plain;
        {
            const Ai::ModelOverride run("quick");
            QCOMPARE(provider.modelFor(plain), QString("small"));
            QCOMPARE(provider.modelFor(request), QString("other"));
            const Ai::ModelOverride inner("named");
            QCOMPARE(provider.modelFor(plain), QString("named"));
        }
        QCOMPARE(provider.modelFor(plain), QString("big"));
        const QJsonObject described = Ai::describe(settings, dir.filePath("no-omarchy"));
        QVERIFY(described.contains("main_model") && described.contains("quick_model"));
    }

    void eachHarnessIsToldTheModelItsOwnWay() {
        QTemporaryDir dir;
        const QByteArray here = dir.path().toUtf8();
        auto recorded = [&](const QString &name) {
            QFile file(dir.filePath(name));
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
        };
        auto ask = [&](AiProvider &provider, AiRequest::Tier tier) {
            AiRequest request;
            request.system = "s";
            request.messages.append(AiMessage{"user", "q"});
            request.tier = tier;
            QSignalSpy finished(&provider, &AiProvider::finished), failed(&provider, &AiProvider::failed);
            provider.ask(request);
            return QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 5000);
        };
        const QByteArray record = "me=$(basename \"$0\")\necho \"$@\" > \"" + here + "/$me.arguments\"\n"
                                  "echo \"$OMABOOK_AI_TIER:$OMABOOK_AI_MODEL\" > \"" + here + "/$me.told\"\n";
        for (const char *harness : {"claude", "codex", "grok", "pi"}) {
            const QString program = script(dir.filePath(harness), record + "cat > /dev/null\necho ok\n");
            const auto provider = CommandProvider::agent(harness, program);
            QVERIFY(provider);
            const QString arguments = QString(harness) + ".arguments";
            // With nothing chosen the harness is left to its own default.
            provider->setModels({"", "small-one"});
            QVERIFY(ask(*provider, AiRequest::Main));
            QVERIFY2(!recorded(arguments).contains("--model"), qPrintable(recorded(arguments)));
            QVERIFY(ask(*provider, AiRequest::Quick));
            QVERIFY2(recorded(arguments).contains("--model small-one"), qPrintable(recorded(arguments)));
            provider->setModels({"big-one", "small-one"});
            QVERIFY(ask(*provider, AiRequest::Main));
            QVERIFY2(recorded(arguments).contains("--model big-one"), qPrintable(recorded(arguments)));
        }
        // Where the prompt comes from stays where the harness looks for it.
        QVERIFY2(recorded("codex.arguments").endsWith("--model big-one -"), qPrintable(recorded("codex.arguments")));
        QVERIFY(recorded("codex.arguments").startsWith("exec "));
        QVERIFY2(recorded("pi.arguments").contains("--model big-one @"), qPrintable(recorded("pi.arguments")));
        QVERIFY(recorded("claude.arguments").startsWith("-p "));

        // Antigravity's models carry their effort, so a model replaces it.
        const QString agy = script(dir.filePath("agy"),
            record + "cat > /dev/null\n"
            "echo '{\"event\":\"step_update\",\"step_update\":{\"step_type\":\"agent_response\",\"text_delta\":\"ok\"}}'\n"
            "echo '{\"event\":\"result\",\"result\":{\"status\":\"SUCCESS\"}}'\n");
        AntigravityProvider antigravity(agy);
        antigravity.setModels({"", "gemini-3.8-flash-medium"});
        QVERIFY(ask(antigravity, AiRequest::Main));
        QVERIFY(recorded("agy.arguments").contains("--effort low") && !recorded("agy.arguments").contains("--model"));
        QVERIFY(ask(antigravity, AiRequest::Quick));
        QVERIFY2(recorded("agy.arguments").contains("--model gemini-3.8-flash-medium") && !recorded("agy.arguments").contains("--effort"),
                 qPrintable(recorded("agy.arguments")));
        QVERIFY(recorded("agy.arguments").contains("--mode plan --sandbox"));

        // A command of the user's own has no flag Omabook knows: it is told
        // the kind of work and the model through its environment.
        CommandProvider own({script(dir.filePath("mine"), record + "cat > /dev/null\necho ok\n"), "--flag"});
        own.setModels({"", "tiny"});
        QVERIFY(ask(own, AiRequest::Main));
        QCOMPARE(recorded("mine.told"), QString("main:"));
        QCOMPARE(recorded("mine.arguments"), QString("--flag"));
        QVERIFY(ask(own, AiRequest::Quick));
        QCOMPARE(recorded("mine.told"), QString("quick:tiny"));
        QCOMPARE(recorded("mine.arguments"), QString("--flag"));

        // The settings reach the provider that is made from them.
        Settings settings(dir.filePath("config.json"));
        settings.setAiProvider("command");
        settings.setAiCommand(dir.filePath("mine"));
        settings.setAiModelFor("command", "quick", "from-settings");
        const auto made = makeAiProvider(settings, nullptr, dir.filePath("no-omarchy"));
        QVERIFY(made);
        QCOMPARE(made->models().quick, QString("from-settings"));
        QVERIFY(ask(*made, AiRequest::Quick));
        QCOMPARE(recorded("mine.told"), QString("quick:from-settings"));
    }

    void aRefusedModelNeverBreaksReading() {
        QTemporaryDir dir;
        const QByteArray here = dir.path().toUtf8();
        // A harness that knows two models and says so of any other.
        const QString claude = script(dir.filePath("claude"),
            "echo \"$@\" >> '" + here + "/runs'\ncat > /dev/null\n"
            "case \" $* \" in\n"
            "  *' --model haiku '*) echo 'quick answer';;\n"
            // As Claude Code does: on standard output, and then an error.
            "  *' --model '*) echo \"There's an issue with the selected model. It may not exist.\"; exit 1;;\n"
            "  *) echo 'main answer';;\n"
            "esac\n");
        auto runs = [&] {
            QFile file(dir.filePath("runs"));
            const QStringList lines = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed().split('\n') : QStringList();
            QFile::remove(dir.filePath("runs"));
            return lines;
        };
        struct Outcome {
            QString answer, error;
            QStringList notes;
        };
        auto ask = [&](AiProvider &provider, AiRequest::Tier tier) {
            Outcome outcome;
            AiRequest request;
            request.messages.append(AiMessage{"user", "q"});
            request.tier = tier;
            const auto pieces = connect(&provider, &AiProvider::delta, this, [&](const QString &text) { outcome.answer += text; });
            const auto notes = connect(&provider, &AiProvider::noted, this, [&](const QString &note) { outcome.notes << note; });
            QSignalSpy finished(&provider, &AiProvider::finished), failed(&provider, &AiProvider::failed);
            provider.ask(request);
            const bool ended = QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 5000);
            if (!failed.isEmpty())
                outcome.error = failed.first().first().toString();
            else if (!ended)
                outcome.error = "The provider neither answered nor failed in time.";
            disconnect(pieces);
            disconnect(notes);
            outcome.answer = outcome.answer.trimmed();
            return outcome;
        };
        const auto provider = CommandProvider::agent("claude", claude);

        // The quick model as it should be: one run, no note.
        provider->setModels({"", "haiku"});
        Outcome outcome = ask(*provider, AiRequest::Quick);
        QCOMPARE(outcome.answer, QString("quick answer"));
        QVERIFY(outcome.notes.isEmpty());
        QCOMPARE(runs().size(), 1);

        // A typo in the quick model: asked once more on the main one, and it says so.
        provider->setModels({"", "hiaku"});
        outcome = ask(*provider, AiRequest::Quick);
        QCOMPARE(outcome.answer, QString("main answer"));
        QCOMPARE(outcome.error, QString());
        QCOMPARE(outcome.notes.size(), 1);
        QVERIFY2(outcome.notes.first().contains("\"hiaku\"") && outcome.notes.first().contains("for reading and parsing")
                     && outcome.notes.first().contains("the default model"),
                 qPrintable(outcome.notes.first()));
        QStringList ran = runs();
        QCOMPARE(ran.size(), 2);
        QVERIFY(ran.first().contains("--model hiaku") && !ran.last().contains("--model"));
        // The main model is what answers, when one is chosen.
        provider->setModels({"haiku", "hiaku"});
        outcome = ask(*provider, AiRequest::Quick);
        QCOMPARE(outcome.answer, QString("quick answer"));
        QVERIFY(outcome.notes.first().contains("\"haiku\" answered instead"));
        runs();

        // A main model that is refused falls back to the harness's default.
        provider->setModels({"opsu", "haiku"});
        outcome = ask(*provider, AiRequest::Main);
        QCOMPARE(outcome.answer, QString("main answer"));
        QVERIFY2(outcome.notes.first().contains("\"opsu\"") && outcome.notes.first().contains("for answers and writing"),
                 qPrintable(outcome.notes.first()));
        runs();

        // A failure that isn't about the model is not asked again: it would
        // only fail again, on a dearer model.
        const QString limited = script(dir.filePath("limited"), "echo \"$@\" >> '" + here + "/runs'\ncat > /dev/null\necho 'rate limited' >&2\nexit 1\n");
        CommandProvider busy({limited});
        busy.setModelFlag("--model");
        busy.setModels({"", "haiku"});
        outcome = ask(busy, AiRequest::Quick);
        QVERIFY(outcome.error.contains("rate limited"));
        QVERIFY(outcome.notes.isEmpty());
        QCOMPARE(runs().size(), 1);

        // Nothing to fall back on: the failure names the model and the work.
        const QString never = script(dir.filePath("never"), "cat > /dev/null\necho 'unknown model' >&2\nexit 1\n");
        CommandProvider stubborn({never});
        stubborn.setModelFlag("--model");
        stubborn.setModels({"", "x1"});
        outcome = ask(stubborn, AiRequest::Quick);
        QVERIFY(outcome.answer.isEmpty());
        QCOMPARE(outcome.notes.size(), 1);
        QVERIFY2(outcome.error.contains("unknown model"), qPrintable(outcome.error));

        // The API says no such model with a 404; the same rule holds there.
        FakeApi api;
        api.status = "404 Not Found";
        api.body = "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\",\"message\":\"model: claude-hiaku-5-5\"}}";
        AnthropicProvider viaApi("k", {}, api.url());
        viaApi.setModels({"claude-opus-5-5", "claude-hiaku-5-5"});
        outcome = ask(viaApi, AiRequest::Quick);
        QCOMPARE(outcome.notes.size(), 2);
        QVERIFY2(outcome.notes.first().contains("\"claude-hiaku-5-5\"") && outcome.notes.first().contains("\"claude-opus-5-5\" answered instead"),
                 qPrintable(outcome.notes.first()));
        QVERIFY2(outcome.error.contains("no such model"), qPrintable(outcome.error));
    }

    void theSettingsPickModelsForTheAiInUse() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        QVERIFY(QDir().mkpath(dir.filePath("bin")) && QDir().mkpath(dir.filePath("omarchy")));
        for (const char *harness : {"claude", "codex"})
            script(dir.filePath(QString("bin/") + harness), "cat\n");
        const QByteArray path = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", path); });
        qputenv("PATH", dir.filePath("bin").toUtf8());
        const QString defaults = dir.filePath("omarchy/agent");
        QVERIFY(Fixtures::write(defaults, "claude\n"));

        AiAgent agent(&settings, defaults);
        QSignalSpy changed(&agent, &AiAgent::changed);
        QCOMPARE(agent.knownModels(), QStringList({"haiku", "sonnet", "opus", "fable"}));
        QCOMPARE(agent.mainChoice(), QString());
        QCOMPARE(agent.mainModel(), QString());
        QCOMPARE(agent.quickDefault(), QString("haiku"));
        QCOMPARE(agent.quickModel(), QString("haiku"));

        agent.setModel("quick", "sonnet");
        QCOMPARE(changed.size(), 1);
        QCOMPARE(agent.quickChoice(), QString("sonnet"));
        QCOMPARE(agent.quickModel(), QString("sonnet"));
        agent.setModel("main", "opus");
        QCOMPARE(agent.mainModel(), QString("opus"));
        QCOMPARE(agent.makeProvider()->models().main, QString("opus"));
        agent.setModel("neither", "x");
        QCOMPARE(changed.size(), 2);

        // Another harness has its own names, and its own choices.
        QVERIFY(Fixtures::write(defaults, "codex\n"));
        QTRY_COMPARE(agent.id(), QString("codex"));
        QVERIFY(agent.knownModels().contains("gpt-6-luna"));
        QCOMPARE(agent.mainChoice(), QString());
        QCOMPARE(agent.quickModel(), QString("gpt-6-luna"));
        // And going back finds what was chosen.
        QVERIFY(Fixtures::write(defaults, "claude\n"));
        QTRY_COMPARE(agent.id(), QString("claude"));
        QCOMPARE(agent.quickModel(), QString("sonnet"));
        agent.setModel("quick", "");
        QCOMPARE(agent.quickModel(), QString("haiku"));
    }

    void promptsCarryTheBookAndKeepItUnspoiled() {
        BookMeta book;
        book.title = "Moby-Dick";
        book.authors = QStringList{"Herman Melville"};
        const QString system = Prompts::companion(book);
        QVERIFY(system.contains("\"Moby-Dick\" by Herman Melville"));
        QVERIFY(system.contains("not read past it"));
        QVERIFY(system.contains("not a request to you"));

        const QString asked = Prompts::question("Call me Ishmael.", " the whale ", " Who is he? ");
        QVERIFY(asked.startsWith("<book_excerpt>\nCall me Ishmael.\n</book_excerpt>"));
        QVERIFY(asked.contains("<selection>\nthe whale\n</selection>"));
        QVERIFY(asked.endsWith("Who is he?"));
        QVERIFY(!Prompts::question("x", "", "q").contains("<selection>"));

        // A text longer than fits keeps its end, where the reader is, and says so.
        const QString huge = QString(Prompts::maximumExcerpt, 'a') + "THE END";
        const Prompts::Excerpt excerpt = Prompts::fit(huge);
        QVERIFY(excerpt.trimmed);
        QCOMPARE(excerpt.text.size(), Prompts::maximumExcerpt);
        QVERIFY(excerpt.text.endsWith("THE END"));
        QVERIFY(!Prompts::fit("short").trimmed);
    }

    void aCatchUpSendsOnlyWhatIsNew() {
        // The first time: everything read so far, and no notes.
        const QString first = Prompts::catchUp({}, "Call me Ishmael.");
        QVERIFY(first.contains("has read so far"));
        QVERIFY(first.contains("<book_excerpt>\nCall me Ishmael.\n</book_excerpt>"));
        QVERIFY(!first.contains("<notes>"));
        // After that: the model's own notes, and only the pages since.
        const QString later = Prompts::catchUp("Ishmael goes to sea.", "The whale appears.");
        QVERIFY(later.contains("<notes>\nIshmael goes to sea.\n</notes>"));
        QVERIFY(later.contains("has read since then"));
        QVERIFY(later.contains("The whale appears."));
        QVERIFY(!later.contains("Call me Ishmael."));
        QVERIFY(later.contains("under 200 words"));
    }

    void aSeriesCarriesItsEarlierBooksForward() {
        const QList<SeriesNotes> earlier{{"Book <One>", "Ann left for sea."}, {"Book Two", "Ann came back."}};
        const QString first = Prompts::catchUp({}, "Ann wakes.", earlier);
        QVERIFY(first.contains("<book title=\"Book &lt;One&gt;\">\nAnn left for sea.\n</book>"));
        QVERIFY(first.indexOf("Ann left for sea.") < first.indexOf("Ann came back."));
        QVERIFY(first.indexOf("</earlier_books>") < first.indexOf("<book_excerpt>"));
        QVERIFY(first.contains("where the earlier books left things"));
        QVERIFY(Prompts::cast({}, "Ann wakes.", earlier).contains("the earlier books included"));
        QVERIFY(Prompts::companion({}, earlier).contains("<earlier_books>"));
        // A book on its own is asked about as before.
        QVERIFY(!Prompts::catchUp({}, "Ann wakes.").contains("earlier"));
        QVERIFY(!Prompts::companion({}).contains("earlier"));
    }

    void earlierBooksNothingIsKnownOfAreLookedUp() {
        // A catalog that knows one of the two books, under a longer title.
        struct Catalog : MetadataSource {
            using MetadataSource::MetadataSource;
            QString name() const override { return "Catalog"; }
            QStringList fields() const override { return {"description"}; }
            void search(const BookMeta &book) override {
                QTimer::singleShot(0, this, [this, book] {
                    MetaCandidate other, right;
                    other.meta.title = "Something Else";
                    other.meta.description = "Not this book.";
                    right.meta.title = "The First Book: A Novel";
                    right.meta.description = "Ann goes to sea.";
                    if (book.title == "the first book")
                        emit found({other, right});
                    else
                        emit failed("unreachable");
                });
            }
        };
        Book first, second;
        first.id = 1;
        first.meta.title = "the first book";
        second.id = 2;
        second.meta.title = "The Second Book";
        BlurbSearch search({first, second}, [](QObject *parent) { return QList<MetadataSource *>{new Catalog(parent)}; });
        QSignalSpy finished(&search, &BlurbSearch::finished);
        QVERIFY(finished.wait(2000));
        QCOMPARE(finished.count(), 1);
        const auto blurbs = finished.first().first().value<QHash<qint64, QString>>();
        QCOMPARE(blurbs.size(), 1);
        QCOMPARE(blurbs.value(1), QString("Ann goes to sea."));
        QVERIFY(!BlurbSearch::sameTitle("Dune", "Emma"));

        // With no catalogs it still answers, and a silent one is not waited for.
        BlurbSearch none({first}, {});
        QSignalSpy nothing(&none, &BlurbSearch::finished);
        QVERIFY(nothing.wait(2000));
        struct Silent : Catalog {
            using Catalog::Catalog;
            void search(const BookMeta &) override {}
        };
        BlurbSearch slow({first}, [](QObject *parent) { return QList<MetadataSource *>{new Silent(parent)}; }, 50);
        QSignalSpy gaveUp(&slow, &BlurbSearch::finished);
        QVERIFY(gaveUp.wait(2000));

        // What is only a description is told apart from notes, and a later
        // book of a series says so to the model.
        QVERIFY(Prompts::seriesSoFar({{"One", "Ann goes to sea.", true}}).contains("<book title=\"One\" about=\"true\">"));
        BookMeta third;
        third.title = "Three";
        third.series = "The Sea";
        third.seriesIndex = 3;
        QVERIFY(Prompts::companion(third).contains("book 3 of the series \"The Sea\""));
    }

    void whoIsWhoIsBuiltUpLikeACatchUp() {
        const QString first = Prompts::cast({}, "Call me Ishmael.");
        QVERIFY(first.contains("has read so far") && !first.contains("<notes>"));
        const QString later = Prompts::cast("Ishmael: the narrator.", "Queequeg arrives.");
        QVERIFY(later.contains("<notes>\nIshmael: the narrator.\n</notes>"));
        QVERIFY(later.contains("Queequeg arrives.") && later.contains("has read since then"));
        QVERIFY(later.contains("Say only what the reader has seen"));
    }

    void theInterfaceIsChangedOnRequestAndPutBack() {
        QTemporaryDir dir;
        Customization custom(dir.filePath("custom"));
        Settings settings(dir.filePath("config.json"));
        QVERIFY(QDir().mkpath(dir.filePath("custom")));
        QVERIFY(Fixtures::write(dir.filePath("custom/keys.json"), "{\"library\": {\"R\": \"read\"}}"));
        custom.check();

        // What the model is given: the files, the settings, the colors and the commands.
        const Prompts::InterfaceState state = InterfaceChanges::state(custom, settings, {{"accent", QColor("#ffd60a")}});
        QVERIFY(state.keys.contains("\"R\""));
        QVERIFY(state.style.isEmpty());
        QCOMPARE(state.settings.value("cover_size").toInt(), 176);
        QCOMPARE(state.colors.value("accent").toString(), QString("#ffd60a"));
        QVERIFY(state.commands.contains("reader.page_down (Space, Right)"));
        const AiRequest request = Prompts::change(state, "make it warmer");
        QVERIFY(request.messages.first().text.endsWith("make it warmer"));
        QVERIFY(request.messages.first().text.contains("<style.json>(there is none)</style.json>"));

        // Its answer is read out of whatever it wrapped it in, and applied.
        const Prompts::InterfaceChange change = Prompts::parseChange(
            "Sure!\n```json\n{\"style\": {\"colors\": {\"accent\": \"#c9954f\"}, \"radius\": 6}, \"keys\": null, "
            "\"settings\": {\"cover_size\": 240, \"justify\": true}, \"say\": \"Warmer now.\"}\n```");
        QVERIFY(change.valid && change.hasStyle && !change.hasKeys);
        QCOMPARE(change.say, QString("Warmer now."));
        QCOMPARE(InterfaceChanges::apply(custom, settings, change), QString());
        QCOMPARE(custom.style().value("colors").toMap().value("accent").value<QColor>(), QColor("#c9954f"));
        QCOMPARE(custom.style().value("radius").toInt(), 6);
        QCOMPARE(settings.coverSize(), 240);
        QVERIFY(settings.justify());
        // The keys were left as they were: R as well as Space.
        QCOMPARE(custom.plainKeys().value("library.read").toStringList(), (QStringList{"Space", "R"}));

        // Undone: the file that wasn't there is gone, the settings are back.
        QVERIFY(InterfaceChanges::canUndo(custom));
        QVERIFY(InterfaceChanges::undo(custom, settings));
        QVERIFY(!QFile::exists(dir.filePath("custom/style.json")));
        QVERIFY(custom.style().isEmpty());
        QCOMPARE(settings.coverSize(), 176);
        QVERIFY(!settings.justify());
        QVERIFY(!InterfaceChanges::canUndo(custom));
        QVERIFY(!InterfaceChanges::undo(custom, settings));

        // A change the files' own checks refuse is not left half made.
        const Prompts::InterfaceChange bad = Prompts::parseChange(
            "{\"style\": {\"colors\": {\"accent\": \"#112233\"}}, \"keys\": {\"reader\": {\"Space\": \"fly\"}}, \"settings\": {\"cover_size\": 9000}}");
        const QString problem = InterfaceChanges::apply(custom, settings, bad);
        QVERIFY2(problem.contains("cover_size") && problem.contains("reader.fly"), qPrintable(problem));
        QVERIFY(custom.style().isEmpty());
        QCOMPARE(custom.plainKeys().value("library.read").toStringList(), (QStringList{"Space", "R"}));
        QCOMPARE(custom.plainKeys().value("reader.page_down").toStringList(), (QStringList{"Space", "Right"}));
        QVERIFY(!QFile::exists(dir.filePath("custom/keys.error")));
        // And what isn't an answer at all changes nothing.
        QVERIFY(!InterfaceChanges::apply(custom, settings, Prompts::parseChange("I can't do that.")).isEmpty());
    }

    void metadataFromTheModelIsReadForgivingly() {
        const AiRequest request = Prompts::metadata("dune_frank.epub", {}, "A beginning is the time…");
        QVERIFY(request.messages.first().text.contains("<file_name>dune_frank.epub</file_name>"));
        QVERIFY(request.messages.first().text.contains("A beginning is the time…"));

        const BookMeta meta = Prompts::parseMetadata(
            "Here you go:\n```json\n{\"title\": \"Dune\", \"authors\": [\"Frank Herbert\"], \"series\": \"Dune\", "
            "\"series_index\": 1, \"published\": 1965, \"language\": \"en\", \"tags\": [\"science fiction\"], "
            "\"description\": \"A desert planet.\"}\n```").meta;
        QCOMPARE(meta.title, QString("Dune"));
        QCOMPARE(meta.authors, QStringList{"Frank Herbert"});
        QCOMPARE(meta.seriesIndex, 1.0);
        QCOMPARE(meta.published, QString("1965"));
        QCOMPARE(meta.tags, QStringList{"science fiction"});
        QVERIFY(Prompts::parseMetadata("I can't tell.").meta.title.isEmpty());
    }

    void theModelIsASourceOfDetails() {
        QTemporaryDir dir;
        CommandProvider provider({script(dir.filePath("model"), "cat > /dev/null\necho '{\"title\": \"Found\", \"authors\": [\"Ann\"]}'\n")});
        AiMetadataSource source(&provider, "x.epub", "opening");
        QSignalSpy found(&source, &MetadataSource::found);
        source.search({});
        QVERIFY(found.wait(5000));
        const auto candidates = found.first().first().value<QList<MetaCandidate>>();
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().meta.title, QString("Found"));
        QCOMPARE(candidates.first().source, QString("model"));
    }

    void openLibraryAnswersAreRead() {
        const auto candidates = OpenLibrarySource::parseAnswer("{\"docs\": [\n"
            "            {\"title\": \"Dune\", \"author_name\": [\"Frank Herbert\"], \"first_publish_year\": 1965, \"publisher\": [\"Chilton\", \"Ace\"],\n"
            "             \"isbn\": [\"0441172717\", \"9780441172719\"], \"subject\": [\"a\", \"b\", \"c\", \"d\", \"e\", \"f\"], \"cover_i\": 11481354},\n"
            "            {\"author_name\": [\"No Title\"]}]}");
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().source, QString("Open Library"));
        QCOMPARE(candidates.first().meta.title, QString("Dune"));
        QCOMPARE(candidates.first().meta.authors, QStringList{"Frank Herbert"});
        QCOMPARE(candidates.first().meta.published, QString("1965"));
        QCOMPARE(candidates.first().meta.publisher, QString("Chilton"));
        QCOMPARE(candidates.first().meta.isbn, QString("9780441172719"));
        QCOMPARE(candidates.first().meta.tags.size(), 5);
        QCOMPARE(candidates.first().coverUrl, QUrl("https://covers.openlibrary.org/b/id/11481354-L.jpg"));
        QVERIFY(OpenLibrarySource::parseAnswer("not json").isEmpty());
    }

    void googleBooksAnswersAreRead() {
        const auto candidates = GoogleBooksSource::parseAnswer("{\"items\": [{\"volumeInfo\": {\n"
            "            \"title\": \"Dune\", \"subtitle\": \"Deluxe\", \"authors\": [\"Frank Herbert\"], \"publisher\": \"Ace\", \"publishedDate\": \"2005-08-02\",\n"
            "            \"description\": \"<p>A <b>desert</b> planet.</p>\", \"language\": \"en\", \"categories\": [\"Fiction\"],\n"
            "            \"industryIdentifiers\": [{\"type\": \"ISBN_10\", \"identifier\": \"0441172717\"}, {\"type\": \"ISBN_13\", \"identifier\": \"9780441172719\"}],\n"
            "            \"imageLinks\": {\"thumbnail\": \"http://books.google.com/x?id=1&edge=curl\"}}}]}");
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().meta.title, QString("Dune: Deluxe"));
        QCOMPARE(candidates.first().meta.description, QString("A desert planet."));
        QCOMPARE(candidates.first().meta.isbn, QString("9780441172719"));
        QCOMPARE(candidates.first().coverUrl, QUrl("https://books.google.com/x?id=1"));
    }

    void appleBooksAndCrossrefAnswersAreRead() {
        const auto apple = AppleBooksSource::parseAnswer(
            "{\"results\": [{\"trackName\": \"Dune\", \"artistName\": \"Frank Herbert\", \"releaseDate\": \"1975-09-01T07:00:00Z\", "
            "\"genres\": [\"Science Fiction\", \"Books\"], \"description\": \"<b>A classic</b> masterpiece.\", "
            "\"artworkUrl100\": \"https://is1-ssl.mzstatic.com/image/thumb/x/100x100bb.jpg\"}]}");
        QCOMPARE(apple.size(), 1);
        QCOMPARE(apple.first().meta.title, QString("Dune"));
        QCOMPARE(apple.first().meta.published, QString("1975-09-01"));
        QCOMPARE(apple.first().meta.description, QString("A classic masterpiece."));
        QCOMPARE(apple.first().meta.tags, QStringList{"Science Fiction"});
        QCOMPARE(apple.first().coverUrl, QUrl("https://is1-ssl.mzstatic.com/image/thumb/x/900x900bb.jpg"));

        const auto crossref = CrossrefSource::parseAnswer(
            "{\"message\": {\"items\": [{\"title\": [\"Conversation and Discourse\"], \"subtitle\": [\"Structure and Interpretation\"], "
            "\"author\": [{\"given\": \"Paul\", \"family\": \"Werth\"}], \"publisher\": \"Routledge\", "
            "\"issued\": {\"date-parts\": [[2022, 4, 6]]}, \"ISBN\": [\"0415000000\", \"9781003291039\"]}]}}");
        QCOMPARE(crossref.size(), 1);
        QCOMPARE(crossref.first().meta.title, QString("Conversation and Discourse: Structure and Interpretation"));
        QCOMPARE(crossref.first().meta.authors, QStringList{"Paul Werth"});
        QCOMPARE(crossref.first().meta.published, QString("2022-04-06"));
        QCOMPARE(crossref.first().meta.isbn, QString("9781003291039"));
    }

    void searchAddressesAreEncodedWhole() {
        // A title with everything a query can trip over; a server refuses an
        // address with a bare space or quote in it.
        BookMeta book;
        book.title = "Tom & Jerry: \"What's Next?\" #2";
        book.authors = QStringList{"Zoë O'Neil"};
        QNetworkAccessManager *none = nullptr;
        const QList<QUrl> addresses{OpenLibrarySource(none).searchUrl(book), GoogleBooksSource(none).searchUrl(book),
                                    AppleBooksSource(none).searchUrl(book), CrossrefSource(none).searchUrl(book)};
        for (const QUrl &address : addresses) {
            const QByteArray encoded = address.toEncoded();
            QVERIFY2(address.isValid(), encoded.constData());
            for (const char *bare : {" ", "\"", "#", "'"})
                QVERIFY2(!encoded.mid(encoded.indexOf('?')).contains(bare), encoded.constData());
            QVERIFY2(encoded.contains("%26"), encoded.constData());
        }
        book.isbn = "9780441172719";
        QVERIFY(GoogleBooksSource(none).searchUrl(book).toEncoded().contains("isbn%3A9780441172719"));
        QVERIFY(CrossrefSource(none).searchUrl(book).toEncoded().contains("isbn%3A9780441172719"));
    }

    void aResultSaysWhatItWouldDo() {
        BookMeta book;
        book.title = "dune";
        book.authors = QStringList{"Frank Herbert"};
        book.publisher = "Ace";
        MetaCandidate candidate;
        candidate.meta.title = "Dune";
        candidate.meta.authors = QStringList{"Frank Herbert"};
        candidate.meta.series = "Dune";
        candidate.meta.description = "A desert planet.";
        candidate.coverUrl = QUrl("https://example.com/cover.jpg");
        const MetaDiff diff = whatItBrings(book, false, candidate);
        // The series, description and cover are new; the title is corrected;
        // the authors are the same and the publisher isn't touched.
        QCOMPARE(diff.adds, (QStringList{"series", "description", "cover"}));
        QCOMPARE(diff.changes.size(), 1);
        QCOMPARE(diff.changes.first().field, QString("title"));
        QCOMPARE(diff.changes.first().from, QString("dune"));
        QCOMPARE(diff.changes.first().to, QString("Dune"));
        // A book that has a cover would have it replaced.
        QCOMPARE(whatItBrings(book, true, candidate).changes.last().field, QString("cover"));
    }

    void aCandidateKeepsItsIdAndItsListing() {
        QTemporaryDir dir;
        const QByteArray cache = qgetenv("XDG_CACHE_HOME");
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        MetaCandidate first;
        first.source = "Apple Books";
        first.meta.title = "Dune";
        first.meta.authors = QStringList{"Frank Herbert"};
        MetaCandidate second = first;
        second.source = "Open Library";
        second.meta.published = "1965";
        // Named by its source and what it says; the AI's reason is not part of it.
        const QString id = BookOps::candidateId(first);
        QVERIFY2(QRegularExpression("^apple-books-[0-9a-f]{8}$").match(id).hasMatch(), qPrintable(id));
        MetaCandidate reasoned = first;
        reasoned.why = "It matches.";
        QCOMPARE(BookOps::candidateId(reasoned), id);
        QVERIFY(BookOps::candidateId(second) != BookOps::candidateId(MetaCandidate(first)));
        MetaCandidate changed = first;
        changed.meta.title = "Dune Messiah";
        QVERIFY(BookOps::candidateId(changed) != id);

        // A listing is kept per library and book, with how it was made.
        Library library(dir.filePath("Library")), other(dir.filePath("Other"));
        QVERIFY(library.open() && other.open());
        QVERIFY(!BookOps::lastListing(library, 7));
        BookOps::rememberListing(library, 7, true, {first, second});
        const auto listed = BookOps::lastListing(library, 7);
        QVERIFY(listed);
        QVERIFY(listed->ai);
        QCOMPARE(listed->ids, (QStringList{id, BookOps::candidateId(second)}));
        QCOMPARE(listed->candidates.at(1).meta.published, QString("1965"));
        QCOMPARE(listed->candidates.at(1).source, QString("Open Library"));
        QVERIFY(!BookOps::lastListing(library, 8));
        QVERIFY(!BookOps::lastListing(other, 7));
        BookOps::forgetListing(library, 7);
        QVERIFY(!BookOps::lastListing(library, 7));
        cache.isEmpty() ? qunsetenv("XDG_CACHE_HOME") : qputenv("XDG_CACHE_HOME", cache);
    }

    void aPluginIsAProgramThatAnswersInJson() {
        QTemporaryDir dir;
        // It is run in its own folder and handed the book on standard input.
        const QString plugin = script(dir.filePath("lookup"),
            "cat > asked.json\n"
            "echo '{\"candidates\": [{\"title\": \"From Plugin\", \"authors\": [\"Pat Plug\"], \"series\": \"Shelf\", "
            "\"series_index\": 2, \"published\": 1999, \"description\": \"<p>Marked <i>up</i>.</p>\", "
            "\"cover_url\": \"https://example.com/c.jpg\"}, {\"authors\": [\"No Title\"]}]}'\n");
        ProcessMetadataSource source("Shelfmark", {"title", "series"}, plugin, {}, dir.path());
        QCOMPARE(source.fields(), (QStringList{"title", "series"}));
        QSignalSpy found(&source, &MetadataSource::found);
        BookMeta book;
        book.title = "Asked About";
        book.isbn = "123";
        source.search(book);
        QVERIFY(found.wait(5000));
        const auto candidates = found.first().first().value<QList<MetaCandidate>>();
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().source, QString("Shelfmark"));
        QCOMPARE(candidates.first().meta.title, QString("From Plugin"));
        QCOMPARE(candidates.first().meta.seriesIndex, 2.0);
        QCOMPARE(candidates.first().meta.published, QString("1999"));
        QCOMPARE(candidates.first().meta.description, QString("Marked up."));
        QCOMPARE(candidates.first().coverUrl, QUrl("https://example.com/c.jpg"));
        QFile asked(dir.filePath("asked.json"));
        QVERIFY(asked.open(QIODevice::ReadOnly));
        const QJsonObject sent = QJsonDocument::fromJson(asked.readAll()).object();
        QCOMPARE(sent.value("title").toString(), QString("Asked About"));
        QCOMPARE(sent.value("isbn").toString(), QString("123"));

        // One that fails says why, in its own words.
        ProcessMetadataSource broken("Broken", {}, script(dir.filePath("broken"), "cat > /dev/null\necho 'no api key set' >&2\nexit 2\n"), {}, dir.path());
        QSignalSpy failed(&broken, &MetadataSource::failed);
        broken.search(book);
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.first().first().toString(), QString("Broken: no api key set"));
        ProcessMetadataSource explained("Polite", {}, script(dir.filePath("polite"), "cat > /dev/null\necho '{\"error\": \"rate limited\"}'\n"), {}, dir.path());
        QSignalSpy explainedFailed(&explained, &MetadataSource::failed);
        explained.search(book);
        QVERIFY(explainedFailed.wait(5000));
        QCOMPARE(explainedFailed.first().first().toString(), QString("Polite: rate limited"));
    }

    void calibresPluginsAreListedSwitchedAndAsked() {
        QTemporaryDir dir;
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        // A stand-in calibre-debug: "-e bridge -- list" lists Calibre's
        // plugins, "... identify" answers for the one it is asked for, both
        // amid the chatter Calibre prints.
        const QString calibre = script(dir.filePath("calibre-debug"),
            "if [ \"$4\" = list ]; then\n"
            "  echo 'Some Calibre chatter'\n"
            "  echo 'OMABOOK-JSON:[{\"name\": \"Amazon.com\", \"version\": \"1.3.19\", \"author\": \"Kovid\", \"description\": \"Amazon\", "
            "\"fields\": [\"title\", \"comments\", \"pubdate\", \"series\", \"identifier:amazon\"], \"identifies\": true, \"enabled\": true, \"configured\": true}, "
            "{\"name\": \"Google\", \"version\": \"1.1\", \"fields\": [\"title\"], \"identifies\": true, \"enabled\": true, \"configured\": true}, "
            "{\"name\": \"Edelweiss\", \"version\": \"2.0\", \"fields\": [\"title\"], \"identifies\": true, \"enabled\": false, \"configured\": true}, "
            "{\"name\": \"Goodreads\", \"version\": \"1.8\", \"fields\": [\"title\", \"series\"], \"identifies\": true, \"enabled\": true, \"configured\": false}, "
            "{\"name\": \"Google Images\", \"version\": \"1.0\", \"fields\": [], \"identifies\": false, \"enabled\": true, \"configured\": true}]'\n"
            "else\n"
            "  cat \"$5\" > '" + dir.path().toUtf8() + "/request.json'\n"
            "  echo 'warning: something'\n"
            "  echo 'OMABOOK-JSON:[{\"plugin\": \"Amazon.com\", \"error\": \"\", \"candidates\": [{\"source\": \"Amazon.com\", "
            "\"title\": \"From Amazon\", \"authors\": [\"A. Zon\"], \"series\": \"Prime\", \"series_index\": 3.0, \"published\": \"2001-02-03\", "
            "\"description\": \"<div>Sold <b>here</b>.</div>\", \"cover_url\": \"https://example.com/a.jpg\"}]}]'\n"
            "fi\n");
        QVERIFY(QDir(dir.path()).mkpath("plugins/mine"));
        QVERIFY(Fixtures::write(dir.filePath("plugins/mine/plugin.json"),
                                "{\"name\": \"Mine\", \"type\": \"metadata\", \"fields\": [\"tags\"], \"command\": [\"./run\", \"--fast\"]}"));
        QVERIFY(QDir(dir.path()).mkpath("plugins/odd"));
        QVERIFY(Fixtures::write(dir.filePath("plugins/odd/plugin.json"), "{\"name\": \"Odd\", \"type\": \"device\"}"));

        Settings settings(dir.filePath("config.json"));
        Plugins plugins(&settings, dir.filePath("plugins"), calibre);
        // Before Calibre has been asked, only what needs no asking is known.
        QVERIFY(!plugins.known().isEmpty());
        QHash<QString, PluginInfo> byId;
        for (const PluginInfo &plugin : plugins.list())
            byId.insert(plugin.id, plugin);

        // Calibre's own: on as Calibre has them, with its field names turned
        // into Omabook's; off where Omabook has the same catalog built in,
        // where Calibre has it off, and unusable until it is set up there.
        // One that only fetches covers isn't a source of details at all.
        QVERIFY(byId.value("calibre:Amazon.com").enabled);
        QCOMPARE(byId.value("calibre:Amazon.com").origin, QString("calibre"));
        QCOMPARE(byId.value("calibre:Amazon.com").fields, (QStringList{"title", "description", "published", "series"}));
        QVERIFY(!byId.value("calibre:Google").enabled);
        QVERIFY(!byId.value("calibre:Edelweiss").enabled);
        QVERIFY(byId.value("calibre:Goodreads").problem.contains("setting up in Calibre"));
        QVERIFY(!byId.contains("calibre:Google Images"));
        // The user's own, and one of a kind this Omabook doesn't know.
        QCOMPARE(byId.value("folder:mine").command, (QStringList{"./run", "--fast"}));
        QVERIFY(byId.value("folder:mine").enabled);
        QVERIFY(byId.value("folder:odd").problem.contains("device"));
        // What Omabook is made of is listed, and can't be switched off.
        QVERIFY(byId.contains("format:PDF") && byId.contains("catalog:CSV") && byId.contains("ai:Claude Code"));
        QVERIFY(!plugins.setEnabled("format:PDF", false));

        // A source for each one that is on and usable.
        QNetworkAccessManager network;
        QObject owner;
        auto names = [&] {
            QStringList names;
            for (MetadataSource *source : plugins.sources(plugins.list(), &network, &owner))
                names << source->name();
            return names;
        };
        QCOMPARE(names(), (QStringList{"Open Library", "Google Books", "Apple Books", "Crossref", "Goodreads", "Mine",
                                       QString::fromUtf8("Amazon.com · Calibre")}));
        // Switching is remembered, and a Calibre plugin can be turned on over the default.
        QVERIFY(plugins.setEnabled("builtin:Crossref", false));
        QVERIFY(plugins.setEnabled("calibre:Google", true));
        QVERIFY(!plugins.setEnabled("calibre:Nope", true));
        Settings reopened(dir.filePath("config.json"));
        QVERIFY(!reopened.pluginEnabled("builtin:Crossref", true));
        QCOMPARE(names(), (QStringList{"Open Library", "Google Books", "Apple Books", "Goodreads", "Mine",
                                       QString::fromUtf8("Amazon.com · Calibre"), QString::fromUtf8("Google · Calibre")}));

        // Asking one runs it inside Calibre and reads its answer out of the chatter.
        MetadataSource *amazon = plugins.sources(plugins.list(), &network, &owner).at(5);
        QSignalSpy found(amazon, &MetadataSource::found);
        BookMeta book;
        book.title = "Wanted";
        amazon->search(book);
        QVERIFY(found.wait(5000));
        const auto candidates = found.first().first().value<QList<MetaCandidate>>();
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().source, QString::fromUtf8("Amazon.com · Calibre"));
        QCOMPARE(candidates.first().meta.title, QString("From Amazon"));
        QCOMPARE(candidates.first().meta.series, QString("Prime"));
        QCOMPARE(candidates.first().meta.description, QString("Sold here."));
        QFile request(dir.filePath("request.json"));
        QVERIFY(request.open(QIODevice::ReadOnly));
        const QJsonObject sent = QJsonDocument::fromJson(request.readAll()).object();
        QCOMPARE(sent.value("title").toString(), QString("Wanted"));
        QCOMPARE(sent.value("plugins").toArray().first().toString(), QString("Amazon.com"));

        // With no Calibre there are simply no Calibre plugins.
        Plugins without(&settings, dir.filePath("plugins"), "-");
        for (const PluginInfo &plugin : without.list())
            QVERIFY(plugin.origin != "calibre");
    }

    void theModelWeighsWhatTheCatalogsFound() {
        MetaCandidate right, wrong;
        right.source = "Open Library";
        right.meta.title = "Dune";
        right.coverUrl = QUrl("https://example.com/dune.jpg");
        wrong.source = "Apple Books";
        wrong.meta.title = "Dune: The Graphic Novel";
        const AiRequest request = Prompts::metadata("dune.epub", {}, "Arrakis...", {right, wrong});
        QVERIFY(request.system.contains("ground truth"));
        QVERIFY(request.system.contains("\"why\""));
        const QString asked = request.messages.first().text;
        QVERIFY(asked.contains("<catalog_results>"));
        QVERIFY(asked.contains("Open Library") && asked.contains("Dune: The Graphic Novel"));
        // Covers are for the reader to see, not for the model to read.
        QVERIFY(!asked.contains("example.com"));
        QVERIFY(!Prompts::metadata("x", {}, "y").messages.first().text.contains("<catalog_results>"));
        // The results are numbered and to be combined field by field, and
        // the model says whose cover to take.
        QVERIFY(request.system.contains("field by field") && request.system.contains("cover_from"));
        QVERIFY(asked.contains("\"result\": 1") && asked.contains("\"has_cover\": true"));
        QCOMPARE(Prompts::parseMetadata("{\"title\": \"Dune\", \"cover_from\": 1}", {right, wrong}).coverUrl, right.coverUrl);
        QVERIFY(!Prompts::parseMetadata("{\"title\": \"Dune\", \"cover_from\": 9}", {right, wrong}).coverUrl.isValid());

        // Its answer carries its reason, and the cover of the result it chose.
        QTemporaryDir dir;
        CommandProvider provider({script(dir.filePath("model"),
            "cat > /dev/null\necho '{\"title\": \"Dune\", \"authors\": [\"Frank Herbert\"], \"why\": \"Matches Open Library.\"}'\n")});
        AiMetadataSource source(&provider, "dune.epub", "Arrakis...");
        source.setFound({wrong, right});
        QSignalSpy found(&source, &MetadataSource::found);
        source.search({});
        QVERIFY(found.wait(5000));
        const auto judged = found.first().first().value<QList<MetaCandidate>>();
        QCOMPARE(judged.first().why, QString("Matches Open Library."));
        QCOMPARE(judged.first().coverUrl, QUrl("https://example.com/dune.jpg"));
    }

    void detailsMergeFieldByField() {
        BookMeta mine;
        mine.title = "dune";
        mine.tags = QStringList{"mine"};
        mine.publisher = "Kept";
        BookMeta found;
        found.title = "Dune";
        found.authors = QStringList{"Frank Herbert"};
        found.series = "Dune";
        found.seriesIndex = 1;
        mine.apply(found);
        QCOMPARE(mine.title, QString("Dune"));
        QCOMPARE(mine.authors, QStringList{"Frank Herbert"});
        QCOMPARE(mine.seriesIndex, 1.0);
        // What the found details don't have stays.
        QCOMPARE(mine.publisher, QString("Kept"));
        QCOMPARE(mine.tags, QStringList{"mine"});

        QCOMPARE(BookMeta::splitAuthors("Ann Lee & Bo Ek; Cy Dee and Di Fa"), (QStringList{"Ann Lee", "Bo Ek", "Cy Dee", "Di Fa"}));
        QCOMPARE(BookMeta::splitAuthors("Lee, Ann"), QStringList{"Lee, Ann"});
        QCOMPARE(BookMeta::splitTags("a, b;c ,, a"), (QStringList{"a", "b", "c"}));
        QCOMPARE(BookMeta::fromJson(mine.toJson()).authors, mine.authors);
    }

    void argumentsAreQuotedForTheOtherShell() {
        QCOMPARE(Remote::quote("plain"), QString("'plain'"));
        QCOMPARE(Remote::quote("The Cartographer's Daughter; rm -rf $HOME"),
                 QString("'The Cartographer'\\''s Daughter; rm -rf $HOME'"));
        QCOMPARE(Remote("laptop:Books/Second Shelf").host(), QString("laptop"));
    }

    void twoComputersAreBroughtInStep() {
        // "There" is another library folder, reached through a stand-in ssh
        // that runs the command here with this build's omabook.
        QTemporaryDir dir;
        const QString ssh = standInSsh(dir);
        const Remote remote("laptop:" + dir.filePath("There's Library"), {ssh});

        QVERIFY(Fixtures::epub(dir.filePath("both.epub"), "On Both"));
        QVERIFY(Fixtures::epub(dir.filePath("theirs.epub"), "Only There"));
        QVERIFY(Fixtures::epub(dir.filePath("gone.epub"), "Removed Here"));
        QVERIFY(Fixtures::write(dir.filePath("mine.txt"), "Only here.\n"));
        QVERIFY(Fixtures::write(dir.filePath("mine.md"), "# Only here\n"));

        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        const qint64 bothHere = here.add(dir.filePath("both.epub")).id;
        here.setProgress(bothHere, {0, 0.5, 25, 1700001000});
        // Removed here, so it must not come back from there.
        QVERIFY(here.remove(here.add(dir.filePath("gone.epub")).id));
        // Only here, with details set by hand, two formats, a cover and a place.
        BookMeta mine;
        mine.title = "Mine: Renamed";
        mine.authors = QStringList{"Me Myself"};
        mine.tags = QStringList{"kept"};
        mine.cover = QImage(40, 60, QImage::Format_RGB32);
        mine.cover.fill(Qt::cyan);
        const qint64 mineHere = here.add(dir.filePath("mine.txt"), mine).id;
        QVERIFY(here.addFormat(mineHere, dir.filePath("mine.md")));
        here.setProgress(mineHere, {0, 0.75, 75, 1700003000});
        {
            Library there(dir.filePath("There's Library"));
            QVERIFY(there.open());
            there.setProgress(there.add(dir.filePath("both.epub")).id, {1, 0.25, 62, 1700002000});
            QVERIFY(there.add(dir.filePath("theirs.epub")).error.isEmpty());
            QVERIFY(there.add(dir.filePath("gone.epub")).error.isEmpty());
        }
        // Removed long before it was added there: there it stays.
        QVERIFY(direct(here, "UPDATE removed SET removed_at = 1700000500"));

        QStringList carried;
        const SyncSummary summary = syncLibrary(here, remote, [&](const QString &title) { carried << title; });
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.received, 1);
        QCOMPARE(summary.sent, 1);
        QCOMPARE(summary.placesTaken, 1);
        QCOMPARE(carried, (QStringList{"Only There", "Mine: Renamed"}));

        // Here: their book arrived with its details and cover, the removed one
        // did not, and the newer reading place was taken.
        QCOMPARE(here.books().size(), 3);
        const QList<Book> arrived = here.books("only there");
        QCOMPARE(arrived.size(), 1);
        QCOMPARE(arrived.first().meta.authors, (QStringList{"Ann Author", "Bo Writer"}));
        QVERIFY(arrived.first().hasCover);
        QVERIFY(here.books("removed").isEmpty());
        QCOMPARE(here.progress(bothHere).place.section, 1);
        QCOMPARE(here.progress(bothHere).updated, qint64(1700002000));

        // There: this side's book arrived as it is here.
        Library there(dir.filePath("There's Library"));
        QVERIFY(there.open());
        QCOMPARE(there.books().size(), 4);
        const QList<Book> sent = there.books("renamed");
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.first().meta.title, QString("Mine: Renamed"));
        QCOMPARE(sent.first().meta.authors, QStringList{"Me Myself"});
        QCOMPARE(sent.first().meta.tags, QStringList{"kept"});
        QCOMPARE(sent.first().formats, (QStringList{"md", "txt"}));
        QCOMPARE(QImage(there.coverPath(sent.first().id)).size(), QSize(40, 60));
        QCOMPARE(there.progress(sent.first().id).updated, qint64(1700003000));
        QCOMPARE(there.progress(sent.first().id).place.fraction, 0.75);

        // Reading on here, then syncing again: only the place moves, to there.
        here.setProgress(bothHere, {1, 0.9, 95, 1700005000});
        const SyncSummary again = syncLibrary(here, remote);
        QCOMPARE(again.received + again.sent + again.placesTaken, 0);
        QCOMPARE(again.placesSent, 1);
        QCOMPARE(there.progress(there.books("on both").first().id).updated, qint64(1700005000));
        QCOMPARE(there.progress(there.books("on both").first().id).percent, 95.0);
        // And once more: nothing left to do.
        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.received + settled.sent + settled.placesTaken + settled.placesSent, 0);
    }

    // Books without a file (a paper copy, a wish, a book read elsewhere)
    // are known by their names elsewhere, else by title and authors: they
    // travel both ways with their shelf, stars and finished day, a book of
    // the same name isn't made twice, and one removed here stays away. A
    // book with a file brings its reading along too.
    // A book a plugin made carries its names elsewhere to the other
    // computer, but never the plugin's mark ("generated"): only the plugin
    // that made a book sets that. The other side takes the rest, and a
    // second sync has nothing left to give.
    void aPluginsMarkNeverTravelsAndTheSyncSettles() {
        QTemporaryDir dir;
        const Remote remote("laptop:" + dir.filePath("There"), {standInSsh(dir)});
        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        QVERIFY(Fixtures::epub(dir.filePath("story.epub"), "A Told Story"));
        const qint64 story = here.add(dir.filePath("story.epub")).id;
        QVERIFY(here.setIdentifier(story, "generated", "teller"));
        QVERIFY(here.setIdentifier(story, "teller", "s123"));
        {
            Library there(dir.filePath("There"));
            QVERIFY(there.open());
        }
        const SyncSummary first = syncLibrary(here, remote);
        QVERIFY2(first.problems.isEmpty(), qPrintable(first.problems.join('\n')));
        QCOMPARE(first.sent, 1);
        Library there(dir.filePath("There"));
        QVERIFY(there.open());
        const QList<Book> arrived = there.books("told story");
        QCOMPARE(arrived.size(), 1);
        QCOMPARE(arrived.first().identifiers.value("teller").toString(), QString("s123"));
        QVERIFY(!arrived.first().identifiers.contains("generated"));
        // Nothing to give the second time.
        const SyncSummary again = syncLibrary(here, remote);
        QVERIFY2(again.problems.isEmpty(), qPrintable(again.problems.join('\n')));
        QCOMPARE(again.sent + again.readingSent + again.detailsSent + again.placesSent, 0);
        // And a mark said there is never taken in here.
        QVERIFY(there.setIdentifier(there.books("told story").first().id, "generated", "other"));
        syncLibrary(here, remote);
        QCOMPARE(here.book(story)->identifiers.value("generated").toString(), QString("teller"));
    }

    void booksWithoutAFileAreSyncedByWhatTheyAreCalled() {
        QTemporaryDir dir;
        const QString ssh = standInSsh(dir);
        const Remote remote("laptop:" + dir.filePath("There"), {ssh});
        const auto entry = [](Library &library, const QString &title, const QString &author, bool paper) {
            BookMeta meta;
            meta.title = title;
            meta.authors = QStringList{author};
            return library.addEntry(meta, paper).id;
        };

        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        const qint64 shelfHere = entry(here, "The Paper One", "Ann Author", true);
        QVERIFY(here.setReading(shelfHere, "read", 4, "2026-03-01"));
        QVERIFY(here.setIdentifier(shelfHere, "goodreads", "4671"));
        const qint64 sameHere = entry(here, "Same Name, Twice", "Bo Writer", false);
        QVERIFY(here.setReading(sameHere, "to-read", 0, {}));
        QVERIFY(here.remove(entry(here, "Thrown Away", "Cy Gone", false)));
        QVERIFY(Fixtures::epub(dir.filePath("filed.epub"), "With A File"));
        const qint64 filedHere = here.add(dir.filePath("filed.epub")).id;
        QVERIFY(here.setReading(filedHere, "read", 5, "2026-04-02"));
        {
            Library there(dir.filePath("There"));
            QVERIFY(there.open());
            const qint64 wished = entry(there, "Only Wished There", "Di Dreamer", false);
            QVERIFY(there.setReading(wished, "to-read", 0, {}));
            QVERIFY(there.setIdentifier(wished, "openlibrary", "OL1W"));
            // The same book as here, written another way: one book, not two.
            QVERIFY(there.setReading(entry(there, "same name twice", "Bo  Writer", false), "", 3, {}));
            QVERIFY(there.setReading(entry(there, "Thrown Away", "Cy Gone", false), "read", 2, {}));
        }
        // Removed here long before it was shelved there: there it stays.
        QVERIFY(direct(here, "UPDATE removed SET removed_at = 1700000500"));

        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.received, 1);
        QCOMPARE(summary.sent, 2);

        // Here: the wish arrived with its shelf and name, the same book took
        // their stars, and the removed one stayed away.
        QCOMPARE(here.books().size(), 4);
        const QList<Book> wished = here.books("only wished");
        QCOMPARE(wished.size(), 1);
        QCOMPARE(wished.first().shelf, QString("to-read"));
        QCOMPARE(wished.first().identifiers.value("openlibrary").toString(), QString("OL1W"));
        QVERIFY(wished.first().formats.isEmpty());
        QCOMPARE(here.book(sameHere)->rating, 3);
        QCOMPARE(here.book(sameHere)->shelf, QString("to-read"));
        QVERIFY(here.books("thrown").isEmpty());

        // There: the paper book and the one with a file arrived with their
        // reading, and nothing came twice.
        Library there(dir.filePath("There"));
        QVERIFY(there.open());
        QCOMPARE(there.books().size(), 5);
        const QList<Book> paper = there.books("paper one");
        QCOMPARE(paper.size(), 1);
        QVERIFY(paper.first().paper);
        QCOMPARE(paper.first().shelf, QString("read"));
        QCOMPARE(paper.first().rating, 4);
        QCOMPARE(paper.first().finished, QString("2026-03-01"));
        QCOMPARE(paper.first().identifiers.value("goodreads").toString(), QString("4671"));
        const QList<Book> filed = there.books("with a file");
        QCOMPARE(filed.size(), 1);
        QCOMPARE(filed.first().rating, 5);
        QCOMPARE(filed.first().finished, QString("2026-04-02"));
        QCOMPARE(there.books("same name").size(), 1);
        QCOMPARE(there.books("same name").first().shelf, QString("to-read"));

        // Once more: nothing left to do.
        const SyncSummary settled = syncLibrary(here, remote);
        QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
        QCOMPARE(settled.received + settled.sent, 0);
        QCOMPARE(here.books().size(), 4);
        QCOMPARE(there.books().size(), 5);
    }

    // Of a book both computers have, the newer details win, either way, with
    // the cover when it is another picture. What is taken or given keeps the
    // time it had, so the next sync finds both alike and does nothing.
    void theNewerDetailsWinEitherWay() {
        QTemporaryDir dir;
        const Remote remote("laptop:" + dir.filePath("There"), {standInSsh(dir)});
        QVERIFY(Fixtures::epub(dir.filePath("a.epub"), "Changed Here"));
        QVERIFY(Fixtures::epub(dir.filePath("b.epub"), "Changed There"));
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 a = here.add(dir.filePath("a.epub")).id, b = here.add(dir.filePath("b.epub")).id;
        const qint64 aThere = there.add(dir.filePath("a.epub")).id, bThere = there.add(dir.filePath("b.epub")).id;
        QVERIFY(direct(here, "UPDATE books SET details_at = 1700001000") && direct(there, "UPDATE books SET details_at = 1700001000"));

        BookMeta mine = here.book(a)->meta;
        mine.title = "Changed Here, Looked Up";
        mine.series = "The Cycle";
        mine.seriesIndex = 2;
        mine.tags = QStringList{"fantasy", "maps"};
        mine.description = "Looked up again.";
        QVERIFY(here.setMeta(a, mine, nullptr, 1700002000));
        BookMeta theirs = there.book(bThere)->meta;
        theirs.description = "Written there.";
        theirs.tags = QStringList{"history"};
        theirs.cover = QImage(50, 70, QImage::Format_RGB32);
        theirs.cover.fill(Qt::darkGreen);
        QVERIFY(there.setMeta(bThere, theirs, nullptr, 1700003000));

        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.detailsSent, 1);
        QCOMPARE(summary.detailsTaken, 1);
        QCOMPARE(summary.detailsUndecided, 0);
        QCOMPARE(summary.received + summary.sent + summary.removedHere + summary.removedThere, 0);

        // There: this side's newer details, with the time they had here.
        const Book given = *there.book(aThere);
        QCOMPARE(given.meta.title, QString("Changed Here, Looked Up"));
        QCOMPARE(given.meta.series, QString("The Cycle"));
        QCOMPARE(given.meta.seriesIndex, 2.0);
        QCOMPARE(given.meta.tags, (QStringList{"fantasy", "maps"}));
        QCOMPARE(given.meta.description, QString("Looked up again."));
        QCOMPARE(given.detailsAt, qint64(1700002000));
        // Here: theirs, cover and all.
        const Book taken = *here.book(b);
        QCOMPARE(taken.meta.description, QString("Written there."));
        QCOMPARE(taken.meta.tags, QStringList{"history"});
        QCOMPARE(taken.detailsAt, qint64(1700003000));
        QCOMPARE(QImage(here.coverPath(b)).size(), QSize(50, 70));

        // In step: nothing goes back and forth.
        const SyncSummary again = syncLibrary(here, remote);
        QVERIFY2(again.problems.isEmpty(), qPrintable(again.problems.join('\n')));
        QCOMPARE(again.detailsTaken + again.detailsSent + again.detailsUndecided + again.readingTaken + again.readingSent, 0);
        QCOMPARE(here.book(b)->detailsAt, qint64(1700003000));
        QCOMPARE(there.book(aThere)->detailsAt, qint64(1700002000));
    }

    // Books from before a library kept when its details changed are 0 on
    // both sides: where they differ nothing decides, so they are counted and
    // left alone, until the user says whose to take.
    void detailsNoTimeDecidesWaitForAChoice() {
        QTemporaryDir dir;
        const Remote remote("laptop:" + dir.filePath("There"), {standInSsh(dir)});
        QVERIFY(Fixtures::epub(dir.filePath("a.epub"), "First Book"));
        QVERIFY(Fixtures::epub(dir.filePath("b.epub"), "Second Book"));
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 a = here.add(dir.filePath("a.epub")).id, b = here.add(dir.filePath("b.epub")).id;
        const qint64 aThere = there.add(dir.filePath("a.epub")).id, bThere = there.add(dir.filePath("b.epub")).id;
        const auto describe = [](Library &library, qint64 id, const QString &description) {
            BookMeta meta = library.book(id)->meta;
            meta.description = description;
            return library.setMeta(id, meta, nullptr, 0);
        };
        QVERIFY(describe(here, a, "Mine.") && describe(here, b, "Mine too."));
        QVERIFY(describe(there, aThere, "Theirs.") && describe(there, bThere, "Theirs too."));

        const SyncSummary unknown = syncLibrary(here, remote);
        QVERIFY2(unknown.problems.isEmpty(), qPrintable(unknown.problems.join('\n')));
        QCOMPARE(unknown.detailsUndecided, 2);
        QCOMPARE(unknown.titles.detailsUndecided, (QStringList{"First Book", "Second Book"}));
        QCOMPARE(unknown.detailsTaken + unknown.detailsSent, 0);
        QCOMPARE(here.book(a)->meta.description, QString("Mine."));
        QCOMPARE(there.book(aThere)->meta.description, QString("Theirs."));

        // --details mine: this side's go there.
        const SyncSummary mine = syncLibrary(here, remote, {}, SyncOptions{.details = SyncOptions::Details::Mine});
        QVERIFY2(mine.problems.isEmpty(), qPrintable(mine.problems.join('\n')));
        QCOMPARE(mine.detailsSent, 2);
        QCOMPARE(mine.detailsUndecided, 0);
        QCOMPARE(there.book(aThere)->meta.description, QString("Mine."));
        QCOMPARE(there.book(bThere)->meta.description, QString("Mine too."));
        QCOMPARE(syncLibrary(here, remote).detailsUndecided, 0);

        // --details theirs: the other side's come here.
        QVERIFY(describe(there, aThere, "Theirs, after all."));
        const SyncSummary theirs = syncLibrary(here, remote, {}, SyncOptions{.details = SyncOptions::Details::Theirs});
        QVERIFY2(theirs.problems.isEmpty(), qPrintable(theirs.problems.join('\n')));
        QCOMPARE(theirs.detailsTaken, 1);
        QCOMPARE(theirs.detailsSent, 0);
        QCOMPARE(here.book(a)->meta.description, QString("Theirs, after all."));
        QCOMPARE(here.book(b)->meta.description, QString("Mine too."));
        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.detailsUndecided + settled.detailsTaken + settled.detailsSent, 0);
    }

    // A book removed on one computer is removed on the other at the next
    // sync, to the trash there, either way; a book without a file too.
    void aRemovalTravelsEitherWayToTheTrash() {
        QTemporaryDir dir;
        const Remote remote("laptop:" + dir.filePath("There"), {standInSsh(dir)});
        const HomeIn home(dir.filePath("here-home"));

        QVERIFY(Fixtures::epub(dir.filePath("x.epub"), "Removed Here"));
        QVERIFY(Fixtures::epub(dir.filePath("y.epub"), "Removed There"));
        QVERIFY(Fixtures::epub(dir.filePath("z.epub"), "Kept By Both"));
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        BookMeta wish;
        wish.title = "Wished Away";
        wish.authors = QStringList{"Di Dreamer"};
        QHash<QString, qint64> mine, theirs;
        for (const char *file : {"x", "y", "z"}) {
            mine.insert(file, here.add(dir.filePath(QString(file) + ".epub")).id);
            theirs.insert(file, there.add(dir.filePath(QString(file) + ".epub")).id);
        }
        const qint64 wishHere = here.addEntry(wish, false).id;
        QVERIFY(there.addEntry(wish, false).error.isEmpty());
        // Both had them long before either removed one.
        for (const Library *library : {&here, &there})
            QVERIFY(direct(*library, "UPDATE books SET added = 1700001000, details_at = 1700001000"));
        QVERIFY(here.remove(mine.value("x")) && here.remove(wishHere));
        QVERIFY(there.remove(theirs.value("y")));

        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.removedHere, 1);
        QCOMPARE(summary.titles.removedHere, QStringList{"Removed There"});
        QCOMPARE(summary.removedThere, 2);
        QCOMPARE(summary.titles.removedThere, (QStringList{"Removed Here", "Wished Away"}));
        QCOMPARE(summary.received + summary.sent, 0);

        // Both hold the one book neither removed, and the rest is in each trash.
        QCOMPARE(here.books().size(), 1);
        QCOMPARE(there.books().size(), 1);
        QCOMPARE(here.books().first().meta.title, QString("Kept By Both"));
        QCOMPARE(there.books().first().meta.title, QString("Kept By Both"));
        const QStringList trashedThere = trashedFrom(dir.filePath("There"), {dir.filePath("there-home")});
        const QStringList trashedHere = trashedFrom(dir.filePath("Here"), {dir.filePath("here-home")});
        QVERIFY2(trashedThere.filter("Removed Here").size() == 1, qPrintable(trashedThere.join(", ")));
        QVERIFY2(trashedHere.filter("Removed There").size() == 1, qPrintable(trashedHere.join(", ")));
        // Removed on both sides now, and nothing more to do.
        QVERIFY(here.wasRemoved(Library::hashFile(dir.filePath("y.epub"))));
        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.removedHere + settled.removedThere + settled.received + settled.sent, 0);
    }

    // A book read, changed or shelved after the other computer removed it
    // is kept where it changed, and not brought back to where it was removed.
    void aBookChangedSinceItWasRemovedIsKept() {
        QTemporaryDir dir;
        const HomeIn home(dir.filePath("here-home"));
        const Remote remote("laptop:" + dir.filePath("There"), {standInSsh(dir)});
        QVERIFY(Fixtures::epub(dir.filePath("read.epub"), "Read Since"));
        QVERIFY(Fixtures::epub(dir.filePath("edited.epub"), "Edited Since"));
        QVERIFY(Fixtures::epub(dir.filePath("shelved.epub"), "Shelved Since"));
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QHash<QString, qint64> mine, theirs;
        for (const char *file : {"read", "edited", "shelved"}) {
            mine.insert(file, here.add(dir.filePath(QString(file) + ".epub")).id);
            theirs.insert(file, there.add(dir.filePath(QString(file) + ".epub")).id);
        }
        for (const Library *library : {&here, &there})
            QVERIFY(direct(*library, "UPDATE books SET added = 1700001000, details_at = 1700001000"));
        QVERIFY(here.remove(mine.value("read")) && here.remove(mine.value("edited")));
        QVERIFY(there.remove(theirs.value("shelved")));
        for (const Library *library : {&here, &there})
            QVERIFY(direct(*library, "UPDATE removed SET removed_at = 1700001500"));
        // Since then: read on there, its details changed there, and put on a shelf here.
        there.setProgress(theirs.value("read"), {.place = {.section = 1, .fraction = 0.5}, .percent = 40, .updated = 1700002000});
        QVERIFY(there.setDetailsAt(theirs.value("edited"), 1700002000));
        QVERIFY(direct(here, QString("UPDATE books SET reading_at = 1700002000 WHERE id = %1").arg(mine.value("shelved"))));

        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.removedHere + summary.removedThere, 0);
        QCOMPARE(summary.received + summary.sent, 0);
        QVERIFY(there.book(theirs.value("read")) && there.book(theirs.value("edited")));
        QVERIFY(here.book(mine.value("shelved")));
        QVERIFY(here.books("read since").isEmpty() && here.books("edited since").isEmpty());
        QVERIFY(there.books("shelved since").isEmpty());
    }

    // A merge (tidy) folds a duplicate into the book kept and removes the
    // duplicate, which the other computer then removes too. But a file the
    // kept book took over from it is still in the library: a book that holds
    // it on the other side is that same book, and is never removed, in
    // either direction.
    void aMergeRemovesTheDuplicateAndNeverTheBookKept() {
        QTemporaryDir dir;
        const HomeIn home(dir.filePath("here-home"));
        const QString ssh = standInSsh(dir);
        QVERIFY(Fixtures::epub(dir.filePath("kept.epub"), "Kept Copy"));
        QVERIFY(Fixtures::epub(dir.filePath("duplicate.epub"), "Duplicate Copy"));
        QVERIFY(Fixtures::write(dir.filePath("other.txt"), "Kept Copy, as text.\n"));
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QHash<QString, qint64> mine, theirs;
        for (const char *file : {"kept.epub", "duplicate.epub", "other.txt"}) {
            mine.insert(file, here.add(dir.filePath(file)).id);
            theirs.insert(file, there.add(dir.filePath(file)).id);
        }
        for (const Library *library : {&here, &there})
            QVERIFY(direct(*library, "UPDATE books SET added = 1700001000, details_at = 1700001000"));
        const qint64 kept = mine.value("kept.epub");
        // The kept book has an EPUB already, so the duplicate's goes with it;
        // the text is a format it lacks, so it takes that over.
        QVERIFY(here.merge(kept, mine.value("duplicate.epub")));
        QVERIFY(here.merge(kept, mine.value("other.txt")));
        QCOMPARE(here.book(kept)->formats, (QStringList{"epub", "txt"}));
        // As an Omabook before 0.1.5 remembered it: the file the kept book took
        // over counted as removed too.
        const QString text = Library::hashFile(dir.filePath("other.txt"));
        QVERIFY(direct(here, QString("INSERT OR REPLACE INTO removed (hash, removed_at) VALUES ('%1', %2)")
                                 .arg(text).arg(QDateTime::currentSecsSinceEpoch())));

        const SyncSummary summary = syncLibrary(here, Remote("laptop:" + dir.filePath("There"), {ssh}));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.removedThere, 1);
        QCOMPARE(summary.titles.removedThere, QStringList{"Duplicate Copy"});
        QCOMPARE(summary.removedHere, 0);
        QVERIFY(!there.book(theirs.value("duplicate.epub")));
        QVERIFY(there.book(theirs.value("kept.epub")));
        QVERIFY(there.book(theirs.value("other.txt")));

        // And from the other computer's side, with this one as the other.
        const SyncSummary back = syncLibrary(there, Remote("desk:" + dir.filePath("Here"), {ssh}));
        QVERIFY2(back.problems.isEmpty(), qPrintable(back.problems.join('\n')));
        QCOMPARE(back.removedHere + back.removedThere, 0);
        QVERIFY(there.book(theirs.value("kept.epub")) && there.book(theirs.value("other.txt")));
        QCOMPARE(here.book(kept)->formats, (QStringList{"epub", "txt"}));
        QCOMPARE(here.books().size(), 1);
    }

    // A dry run changes nothing on either side, and says, book by book, what
    // a sync would do; the sync then does just that.
    void aDryRunChangesNothing() {
        QTemporaryDir dir;
        const HomeIn home(dir.filePath("here-home"));
        const Remote remote("laptop:" + dir.filePath("There"), {standInSsh(dir)});
        QVERIFY(Fixtures::epub(dir.filePath("mine.epub"), "Only Here"));
        QVERIFY(Fixtures::epub(dir.filePath("theirs.epub"), "Only There"));
        QVERIFY(Fixtures::epub(dir.filePath("both.epub"), "On Both"));
        QVERIFY(Fixtures::epub(dir.filePath("gone.epub"), "Removed Here"));
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QVERIFY(here.add(dir.filePath("mine.epub")).error.isEmpty());
        QVERIFY(there.add(dir.filePath("theirs.epub")).error.isEmpty());
        const qint64 both = here.add(dir.filePath("both.epub")).id, bothThere = there.add(dir.filePath("both.epub")).id;
        const qint64 gone = here.add(dir.filePath("gone.epub")).id;
        QVERIFY(there.add(dir.filePath("gone.epub")).error.isEmpty());
        for (const Library *library : {&here, &there})
            QVERIFY(direct(*library, "UPDATE books SET added = 1700001000, details_at = 1700001000"));
        BookMeta newer = here.book(both)->meta;
        newer.description = "Newer here.";
        QVERIFY(here.setMeta(both, newer, nullptr, 1700002000));
        QVERIFY(here.setReading(both, "reading", 4, {}));
        there.setProgress(bothThere, {.place = {.section = 1, .fraction = 0.5}, .percent = 50, .updated = 1700003000});
        QVERIFY(here.remove(gone));
        const QByteArray before = snapshot(here), beforeThere = snapshot(there);

        const SyncSummary dry = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QVERIFY2(dry.error.isEmpty(), qPrintable(dry.error));
        QVERIFY2(dry.problems.isEmpty(), qPrintable(dry.problems.join('\n')));
        QVERIFY(dry.dryRun);
        QCOMPARE(dry.titles.received, QStringList{"Only There"});
        QCOMPARE(dry.titles.sent, QStringList{"Only Here"});
        QCOMPARE(dry.titles.detailsSent, QStringList{"On Both"});
        QCOMPARE(dry.titles.placesTaken, QStringList{"On Both"});
        QCOMPARE(dry.titles.readingSent, QStringList{"On Both"});
        QCOMPARE(dry.titles.removedThere, QStringList{"Removed Here"});
        QVERIFY(dry.titles.removedHere.isEmpty() && dry.titles.detailsTaken.isEmpty());
        QCOMPARE(snapshot(here), before);
        QCOMPARE(snapshot(there), beforeThere);
        QCOMPARE(here.books().size(), 2);
        QCOMPARE(there.books().size(), 3);

        // The sync itself does what the dry run said.
        const SyncSummary done = syncLibrary(here, remote);
        QVERIFY2(done.problems.isEmpty(), qPrintable(done.problems.join('\n')));
        QCOMPARE(done.titles.received, dry.titles.received);
        QCOMPARE(done.titles.sent, dry.titles.sent);
        QCOMPARE(done.titles.detailsSent, dry.titles.detailsSent);
        QCOMPARE(done.titles.placesTaken, dry.titles.placesTaken);
        QCOMPARE(done.titles.readingSent, dry.titles.readingSent);
        QCOMPARE(done.titles.removedThere, dry.titles.removedThere);
        QCOMPARE(there.book(bothThere)->meta.description, QString("Newer here."));
        QCOMPARE(here.progress(both).updated, qint64(1700003000));
        QVERIFY(there.books("removed here").isEmpty());
    }

    void aComputerThatCannotBeReachedSaysSo() {
        QTemporaryDir dir;
        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        // A computer on a newer Omabook, whose manifest this one could misread, is
        // not synced with: it says which side to update and changes nothing.
        const qsizetype before = here.books().size();
        const SyncSummary newer = syncLibrary(here, Remote("laptop", {script(dir.filePath("newer"),
                                                           "echo '{\"version\": 99, \"omabook\": \"9.0.0\", \"books\": [], \"removed\": [\"x\"]}'\n")}));
        QVERIFY2(newer.error.contains("newer Omabook (9.0.0)") && newer.error.contains("updated"), qPrintable(newer.error));
        QCOMPARE(here.books().size(), before);
        QVERIFY(!here.wasRemoved("x"));
        const SyncSummary off = syncLibrary(here, Remote("laptop", {script(dir.filePath("off"), "echo 'ssh: connect to host laptop: No route to host' >&2\nexit 255\n")}));
        QVERIFY2(off.error.contains("No route to host"), qPrintable(off.error));
        const SyncSummary missing = syncLibrary(here, Remote("laptop", {script(dir.filePath("none"), "echo 'sh: omabook: command not found' >&2\nexit 127\n")}));
        QVERIFY2(missing.error.contains("isn't installed on laptop"), qPrintable(missing.error));
        QVERIFY(here.books().isEmpty());
    }

    void conversionRunsTheToolAndReportsIt() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("in.mobi"), "kindle"));
        Converter converter(script(dir.filePath("convert"), "cp \"$1\" \"$2\"\n"));
        QVERIFY(converter.available());
        QSignalSpy finished(&converter, &Converter::finished);
        converter.convert(dir.filePath("in.mobi"), dir.filePath("out.epub"));
        QVERIFY(finished.wait(5000));
        QCOMPARE(finished.first().at(1).toString(), QString());
        QVERIFY(QFile::exists(dir.filePath("out.epub")));

        // A tool that fails leaves no half-written book, and its reason is passed on.
        Converter failing(script(dir.filePath("fail"), "echo partial > \"$2\"\necho 'Cannot read MOBI: DRM' >&2\nexit 1\n"));
        QSignalSpy failed(&failing, &Converter::finished);
        failing.convert(dir.filePath("in.mobi"), dir.filePath("bad.epub"));
        QVERIFY(failed.wait(5000));
        QVERIFY2(failed.first().at(1).toString().contains("DRM"), qPrintable(failed.first().at(1).toString()));
        QVERIFY(!QFile::exists(dir.filePath("bad.epub")));
    }
};

QTEST_MAIN(ServicesTests)
#include "services_tests.moc"
