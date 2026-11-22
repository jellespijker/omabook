// The AI page of Settings (#144, ADR 0008): a row per capability, each with
// its provider and model, whether it may be hosted, and why it can't be used
// with what to do; every row worked with a fake provider (a server on this
// computer, a whisper.cpp and a speech-dispatcher that are scripts), by the
// keyboard and the controls a finger taps, in English and in German; and
// text to speech's voice, pace and preview, OpenAI's voices among them (#147).

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QtTest>
#include <utility>

#include "aicapability.h"
#include "aiendpoint.h"
#include "desk.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "secretstore.h"

using namespace Qt::StringLiterals;

namespace {

// Answers what a server is asked on the AI page: its models, and embeddings.
FakeServer::Reply answers(const FakeServer::Request &request) {
    if (request.line.startsWith("GET"))
        return {.status = "200 OK", .body = R"({"data": [{"id": "gemma3"}, {"id": "nomic-embed-text"}]})"};
    QJsonArray data;
    const QJsonArray input = request.json().value("input").toArray();
    for (qsizetype at = 0; at < input.size(); ++at)
        data.append(QJsonObject{{"index", at}, {"embedding", QJsonArray{3.0, 4.0}}});
    return {.status = "200 OK", .body = QJsonDocument(QJsonObject{{"data", data}}).toJson(QJsonDocument::Compact)};
}

[[nodiscard]] bool program(const QString &path, const QByteArray &script) {
    if (!Fixtures::write(path, script))
        return false;
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

[[nodiscard]] QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class AiPageTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;

    static QVariant value(const Desk &desk, const QString &name, const char *property) {
        QObject *found = desk.find(name);
        return found ? found->property(property) : QVariant();
    }
    static QString text(const Desk &desk, const QString &name) { return value(desk, name, "text").toString(); }
    static QJsonObject capability(const Desk &desk, const char *name) { return desk.settings->capabilities().value(name).toObject(); }
    static bool open(Desk &desk) {
        desk.call("showAiSettings");
        return desk.waitForPopup() && value(desk, "aiSettings", "opened").toBool();
    }
    static void tick(const Desk &desk, const QString &name, bool on) {
        QObject *box = desk.find(name);
        QVERIFY2(box, qPrintable(name));
        box->setProperty("checked", on);
        QVERIFY(QMetaObject::invokeMethod(box, "toggled"));
    }
    static void type(const Desk &desk, const QString &name, const QString &typed) {
        QObject *field = desk.find(name);
        QVERIFY2(field, qPrintable(name));
        field->setProperty("text", typed);
        QVERIFY(QMetaObject::invokeMethod(field, "editingFinished"));
    }
    static void pick(const Desk &desk, const QString &name, int index) {
        QObject *box = desk.find(name);
        QVERIFY2(box, qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(box, "activated", Q_ARG(int, index)));
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

    // Every capability with a provider of its own on this computer: the AI
    // server (a fake one), whisper.cpp, voxtype and speech-dispatcher (scripts), on a
    // new install, local for every capability and hosted for none; the page open.
    static bool fakeProviders(Desk &desk, const FakeServer &server) {
        const bool made = QDir().mkpath(desk.dir.filePath("bin")) && program(desk.dir.filePath("bin/spd-say"), "#!/bin/bash\nexit 0\n")
                          && program(desk.dir.filePath("bin/whisper-cli"), "#!/bin/bash\necho heard\n")
                          && program(desk.dir.filePath("bin/voxtype"), "#!/bin/bash\necho heard\n")
                          && Fixtures::write(desk.dir.filePath("ggml-base.bin"), "model");
        qputenv("PATH", desk.dir.filePath("bin").toUtf8());
        desk.settings->setCapabilities(Capabilities::fresh());
        desk.settings->setAiEndpoint({{"preset", "custom"}, {"base_url", server.url()}});
        desk.settings->setRecognition({{"ink_model", "gemma3"},
                                       {"whisper_program", desk.dir.filePath("bin/whisper-cli")},
                                       {"whisper_model", desk.dir.filePath("ggml-base.bin")}});
        return made && desk.show() && open(desk);
    }

    QByteArray m_path;

private slots:
    void init() { m_path = qgetenv("PATH"); }
    void cleanup() { qputenv("PATH", m_path); }
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        Secrets::useInstead(&m_secrets);
    }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    // Settings has the way to the page; the page says which agent answers and
    // why, follows Omarchy's default while it is open, and has a model for
    // what is read and a smaller one for what Omabook reads.
    void thePageSaysWhichAgentAnswersAndWhy() {
        Desk desk("en", {}, false);
        QVERIFY(QDir().mkpath(desk.dir.filePath("bin")) && QDir().mkpath(desk.dir.filePath("omarchy/defaults")));
        for (const char *harness : {"codex", "claude"})
            QVERIFY(program(desk.dir.filePath(QString("bin/") + harness), "#!/bin/bash\ncat\n"));
        qputenv("PATH", desk.dir.filePath("bin").toUtf8());
        QVERIFY(Fixtures::write(desk.dir.filePath("omarchy/defaults/agent"), "codex\n"));
        QVERIFY(desk.show());
        desk.call("showSettings");
        QVERIFY(desk.waitForPopup());
        // The old dialog's AI part is gone: a line, and the way here.
        QVERIFY(!desk.find("aiProvider") && !desk.find("recognitionHosted") && !desk.find("meaningHosted"));
        QVERIFY(text(desk, "aiSummary").contains(desk.assistant->providerName()));
        QVERIFY(QMetaObject::invokeMethod(desk.find("openAi"), "clicked"));
        QTRY_VERIFY(value(desk, "aiSettings", "opened").toBool());
        QVERIFY(!value(desk, "settings", "visible").toBool());
        QTRY_COMPARE(text(desk, "aiAgent"), QString("Omarchy's default: Codex"));
        // Switching the desktop's default switches Omabook's, with the page open.
        QVERIFY(Fixtures::write(desk.dir.filePath("omarchy/defaults/agent"), "crush\n"));
        QTRY_COMPARE(text(desk, "aiAgent"),
                     QString("Omabook can't run Omarchy's default, Crush, with its tools off, so Claude Code answers."));
        QTRY_VERIFY(text(desk, "answersSummary").startsWith("Claude Code"));
        desk.settings->setAiProvider("claude");
        QTRY_COMPARE(text(desk, "aiAgent"), QString("Chosen here: Claude Code"));

        // The model picker: one model for what is read (answers' row), a
        // smaller one for what Omabook reads (quick work's).
        QObject *quick = desk.find("quickModel"), *main = desk.find("mainModel");
        QVERIFY(quick && main);
        QCOMPARE(main->property("editText").toString(), QString("The default"));
        QCOMPARE(quick->property("editText").toString(), QString("The default (haiku)"));
        QCOMPARE(quick->property("model").toStringList(), QStringList({"The default (haiku)", "haiku", "sonnet", "opus", "fable"}));
        // Picked from the list.
        QVERIFY(QMetaObject::invokeMethod(quick, "activated", Q_ARG(int, 2)));
        QCOMPARE(desk.settings->aiModelFor("claude", "quick"), QString("sonnet"));
        QTRY_COMPARE(quick->property("editText").toString(), QString("sonnet"));
        QTRY_VERIFY(text(desk, "quickSummary").contains("sonnet"));
        // Or typed: any name the AI takes will do.
        QVERIFY(main->setProperty("editText", " claude-opus-5-5 "));
        QVERIFY(QMetaObject::invokeMethod(main, "accepted"));
        QCOMPARE(desk.settings->aiModelFor("claude", "main"), QString("claude-opus-5-5"));
        QTRY_COMPARE(main->property("editText").toString(), QString("claude-opus-5-5"));
        // Back to the default, by its first entry.
        QVERIFY(QMetaObject::invokeMethod(quick, "activated", Q_ARG(int, 0)));
        QCOMPARE(desk.settings->aiModelFor("claude", "quick"), QString());
        // Another AI brings its own names, and nothing chosen for it yet.
        QVERIFY(Fixtures::write(desk.dir.filePath("omarchy/defaults/agent"), "codex\n"));
        desk.settings->setAiProvider("auto");
        QTRY_COMPARE(quick->property("editText").toString(), QString("The default (gpt-6-luna)"));
        QCOMPARE(main->property("editText").toString(), QString("The default"));
        desk.shot("ai-page-models");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Every row ready with a fake provider, on the person's own computers,
    // and gone through by the keyboard.
    void eachRowIsReadyWithAFakeProvider() {
        FakeServer server;
        server.answering = answers;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        // Every row ready, on the person's own computers.
        for (const char *row : {"answers", "quick", "handwriting", "speech-to-text", "text-to-speech", "embeddings"})
            QTRY_VERIFY2(text(desk, QString(row) + "Summary").endsWith("your own computers"),
                         qPrintable(text(desk, QString(row) + "Summary")));
        QVERIFY(text(desk, "handwritingSummary").contains("gemma3"));
        QVERIFY(text(desk, "embeddingsSummary").contains("nomic-embed-text"));
        QVERIFY(text(desk, "text-to-speechSummary").startsWith("speech-dispatcher"));

        // The keyboard: the first row has the focus, Down goes to the next,
        // Enter opens it.
        QTRY_VERIFY(value(desk, "answersRow", "activeFocus").toBool());
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTRY_VERIFY(value(desk, "quickRow", "activeFocus").toBool());
        QTest::keyClick(desk.window, Qt::Key_J);
        QTRY_VERIFY(value(desk, "handwritingRow", "activeFocus").toBool());
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_VERIFY(desk.shows("handwritingHosted"));
        QTest::keyClick(desk.window, Qt::Key_K);
        QTRY_VERIFY(value(desk, "quickRow", "activeFocus").toBool());

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void answersAndQuickWorkAreAllowedHostedEachInTheirRow() {
        FakeServer server;
        server.answering = answers;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        // Answers: a command is someone else's computer for all that can be
        // known, and choosing it allows nothing by itself.
        desk.settings->setAiCommand("cat");
        pick(desk, "answersProvider", 5);
        QCOMPARE(desk.settings->aiProvider(), QString("command"));
        QVERIFY(!capability(desk, "answers").value("hosted").toBool());
        QTRY_COMPARE(text(desk, "answersSummary"), QString("Only a hosted service is set up, and hosted isn't allowed for it"));
        QVERIFY(text(desk, "answersWhy").endsWith("Allow a hosted service for it, or choose one of your own."));
        QVERIFY(QMetaObject::invokeMethod(desk.find("answersAllow"), "clicked"));
        QTRY_VERIFY(text(desk, "answersSummary").endsWith("someone else's computer"));
        QVERIFY(capability(desk, "answers").value("hosted").toBool());
        // Quick work has a hosted choice of its own.
        QVERIFY(!capability(desk, "quick").value("hosted").toBool());
        QCOMPARE(text(desk, "quickSummary"), QString("Only a hosted service is set up, and hosted isn't allowed for it"));
        tick(desk, "quickHosted", true);
        QTRY_VERIFY(text(desk, "quickSummary").endsWith("someone else's computer"));
        QVERIFY(text(desk, "quickHostedNote").startsWith("Careful"));
        pick(desk, "answersProvider", 4);
        QCOMPARE(desk.settings->aiProvider(), QString("openai"));
        QTRY_VERIFY(text(desk, "answersSummary").endsWith("your own computers"));

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Speech, the meaning index, handwriting and voices, each set up in its
    // row, and hosted allowed for one alone.
    void theOtherRowsAreSetUpAndAllowedApart() {
        FakeServer server;
        server.answering = answers;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        // Speech: whisper.cpp named in config.json keeps the server first (#146).
        QVERIFY(text(desk, "speech-to-textSummary").startsWith("the transcription server at 127.0.0.1"));
        // voxtype alone, which gives only the words.
        pick(desk, "speech-to-textProvider", 1);
        QCOMPARE(capability(desk, "speech-to-text").value("provider").toString(), QString("voxtype"));
        QTRY_VERIFY(text(desk, "speech-to-textSummary").startsWith("voxtype on this computer"));
        QVERIFY(text(desk, "speech-to-textSummary").contains("no word timestamps"));
        QTRY_VERIFY(text(desk, "speechHeardBy").contains("doesn't give each word its moment"));
        // whisper.cpp alone, not the server first.
        pick(desk, "speech-to-textProvider", 2);
        QCOMPARE(capability(desk, "speech-to-text").value("provider").toString(), QString("whisper.cpp"));
        QTRY_VERIFY(text(desk, "speech-to-textSummary").startsWith("whisper.cpp on this computer · ggml-base"));
        QVERIFY(text(desk, "speech-to-textSummary").endsWith("your own computers"));
        QVERIFY(!text(desk, "speech-to-textSummary").contains("no word timestamps"));
        // Automatic again: the first there is.
        pick(desk, "speech-to-textProvider", 0);
        QVERIFY(!capability(desk, "speech-to-text").contains("provider"));

        // Embeddings at a hosted address: refused until allowed for it, and
        // only for it.
        type(desk, "embeddingsAddress", "https://api.example.com/v1");
        QCOMPARE(desk.settings->meaning().value("url").toString(), QString("https://api.example.com/v1"));
        QTRY_COMPARE(text(desk, "embeddingsSummary"), QString("Only a hosted service is set up, and hosted isn't allowed for it"));
        QVERIFY(text(desk, "embeddingsWhy").contains("api.example.com"));
        QVERIFY(desk.find("embeddingsKey"));
        // Its key goes to the keyring, never config.json.
        desk.find("aiSettings")->setProperty("current", QString("embeddings"));
        desk.find("embeddingsKey")->setProperty("text", "sk-secret");
        QVERIFY(QMetaObject::invokeMethod(desk.find("embeddingsKey"), "accepted"));
        QVERIFY(!contents(desk.dir.filePath("config.json")).contains("sk-secret"));
        QCOMPARE(AiEndpoints::keptKey("https://api.example.com/v1", m_secrets), QString("sk-secret"));
        QTRY_VERIFY(value(desk, "embeddingsForgetKey", "visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(desk.find("embeddingsAllow"), "clicked"));
        QTRY_VERIFY(text(desk, "embeddingsSummary").endsWith("someone else's computer"));
        QVERIFY(desk.settings->meaningHosted());
        QVERIFY(!desk.settings->recognitionHosted());
        for (const char *other : {"handwriting", "speech-to-text", "text-to-speech"})
            QVERIFY2(!capability(desk, other).value("hosted").toBool(), other);
        // A model typed in.
        type(desk, "embeddingsModel", "mxbai-embed-large");
        QCOMPARE(desk.settings->meaning().value("model").toString(), QString("mxbai-embed-large"));
        QTRY_VERIFY(text(desk, "embeddingsSummary").contains("mxbai-embed-large"));

        // Handwriting alone allowed hosted leaves the older key, which speech
        // shares, at no: never more than was allowed.
        tick(desk, "handwritingHosted", true);
        QVERIFY(capability(desk, "handwriting").value("hosted").toBool());
        QVERIFY(!capability(desk, "speech-to-text").value("hosted").toBool());
        QVERIFY(!desk.settings->recognitionHosted());
        // And setting up recognition doesn't undo it.
        type(desk, "handwritingModel", "llava");
        QCOMPARE(desk.settings->recognition().value("ink_model").toString(), QString("llava"));
        QVERIFY(capability(desk, "handwriting").value("hosted").toBool());

        // Text to speech: a server, the AI server where no address is given.
        pick(desk, "text-to-speechProvider", 3);
        QCOMPARE(desk.settings->readAloud().value("engine").toString(), QString("server"));
        QCOMPARE(capability(desk, "text-to-speech").value("provider").toString(), QString("server"));
        QTRY_VERIFY(text(desk, "text-to-speechSummary").startsWith("127.0.0.1"));

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Gemini with an API key (#145) in every row: its key given once, in any
    // of them, and each capability allowed hosted on its own.
    void geminiIsOfferedInEveryRow() {
        FakeServer server;
        server.answering = answers;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        const QList<std::pair<QString, int>> rows{
            {"answers", 6}, {"handwriting", 1}, {"speech-to-text", 4}, {"text-to-speech", 5}, {"embeddings", 1}};
        for (const auto &[row, index] : rows) {
            desk.find("aiSettings")->setProperty("current", row);
            pick(desk, row + "Provider", index);
        }
        QCOMPARE(desk.settings->aiProvider(), QString("gemini"));
        for (const char *row : {"handwriting", "speech-to-text", "embeddings"})
            QCOMPARE(capability(desk, row).value("provider").toString(), QString("gemini"));
        QCOMPARE(desk.settings->readAloud().value("engine").toString(), QString("gemini"));
        // Choosing it allows nothing, and it needs its key.
        QTRY_COMPARE(text(desk, "answersSummary"), QString("A key is needed"));
        QTRY_COMPARE(text(desk, "handwritingSummary"), QString("Only a hosted service is set up, and hosted isn't allowed for it"));
        desk.find("aiSettings")->setProperty("current", QString("answers"));
        QTRY_VERIFY(desk.shows("answersGeminiKey"));
        desk.find("answersGeminiKey")->setProperty("text", "gemini-secret");
        QVERIFY(QMetaObject::invokeMethod(desk.find("answersGeminiKey"), "accepted"));
        QCOMPARE(m_secrets.get("gemini"), QString("gemini-secret"));
        QVERIFY(!contents(desk.dir.filePath("config.json")).contains("gemini-secret"));
        QTRY_COMPARE(text(desk, "answersSummary"), QString("Only a hosted service is set up, and hosted isn't allowed for it"));
        // Each allowed on its own.
        for (const char *row : {"answers", "quick", "handwriting", "speech-to-text", "text-to-speech", "embeddings"}) {
            desk.find("aiSettings")->setProperty("current", QString(row));
            tick(desk, QString(row) + "Hosted", true);
            QTRY_VERIFY2(text(desk, QString(row) + "Summary").startsWith("Gemini · gemini-"),
                         qPrintable(text(desk, QString(row) + "Summary")));
            QVERIFY(text(desk, QString(row) + "Summary").endsWith("someone else's computer"));
            // The check mark is drawn as it is ticked: the picture waits for it.
            QTest::qWait(300);
            desk.shot(QString("ai-page-gemini-") + row);
        }
        QVERIFY(text(desk, "quickSummary").contains("gemini-3.5-flash-lite"));
        QVERIFY(text(desk, "speech-to-textSummary").contains("gemini-3.5-transcribe"));
        QVERIFY(!text(desk, "speech-to-textSummary").contains("no word timestamps"));
        // Its key is shown as kept in every row Gemini is chosen in, and a model typed in is Gemini's own.
        QTRY_VERIFY(value(desk, "embeddingsForgetGeminiKey", "visible").toBool());
        QVERIFY(!desk.shows("embeddingsAddress"));
        type(desk, "embeddingsModel", "gemini-embedding-2");
        QCOMPARE(desk.settings->gemini().value("models").toObject().value("embeddings").toString(), QString("gemini-embedding-2"));
        QVERIFY(!desk.settings->meaning().contains("model"));
        QTRY_VERIFY(text(desk, "embeddingsSummary").contains("gemini-embedding-2"));
        // Forgotten, and every row needs it again.
        QVERIFY(QMetaObject::invokeMethod(desk.find("embeddingsForgetGeminiKey"), "clicked"));
        QTRY_COMPARE(text(desk, "handwritingSummary"), QString("A key is needed"));
        QVERIFY(m_secrets.get("gemini").isEmpty());

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void offStopsEveryModelButALocalVoice() {
        FakeServer server;
        server.answering = answers;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        // Off stops every model, a voice that is a server's too; a voice on
        // this computer still reads.
        pick(desk, "text-to-speechProvider", 3);
        tick(desk, "aiOn", false);
        QCOMPARE(desk.settings->aiProvider(), QString("off"));
        for (const char *row : {"answers", "quick", "handwriting", "speech-to-text", "text-to-speech", "embeddings"})
            QTRY_COMPARE(text(desk, QString(row) + "Summary"), QString("Off, as the AI is"));
        pick(desk, "text-to-speechProvider", 2);
        QTRY_VERIFY(text(desk, "text-to-speechSummary").startsWith("speech-dispatcher"));
        // On again: answers come back to the provider they had.
        tick(desk, "aiOn", true);
        QCOMPARE(desk.settings->aiProvider(), QString("openai"));
        QTRY_VERIFY(text(desk, "answersSummary").endsWith("your own computers"));
        desk.shot("ai-page-rows");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Text to speech's row: the voice and pace of the provider in use, kept
    // for each; a sentence to hear it by; OpenAI's voices with their key; and
    // the row laid out in a long language, under fingers and right to left.
    void theVoiceIsChosenAndHeardInItsRow() {
        FakeServer server;
        server.answering = answers;
        Desk desk("en", {}, false);
        QVERIFY(fakeProviders(desk, server));
        const QString log = desk.dir.filePath("said.log");
        QVERIFY(program(desk.dir.filePath("bin/spd-say"), "#!/bin/bash\necho \"$*\" >> '" + log.toUtf8() + "'\n"));
        desk.find("aiSettings")->setProperty("current", QString("text-to-speech"));
        pick(desk, "text-to-speechProvider", 2);
        QTRY_VERIFY(desk.shows("voiceChoice"));
        QCOMPARE(value(desk, "voiceChoice", "count").toInt(), 9); // automatic, and speech-dispatcher's eight
        pick(desk, "voiceChoice", 2);
        QVERIFY(QMetaObject::invokeMethod(desk.find("voiceSlower"), "clicked"));
        QTRY_COMPARE(text(desk, "voiceRate"), QString("0.9×"));
        const QJsonObject dispatcher = desk.settings->readAloud().value("voices").toObject().value("speech-dispatcher").toObject();
        QCOMPARE(dispatcher.value("voice").toString(), QString("female2"));
        QCOMPARE(dispatcher.value("rate").toDouble(), 0.9);
        // Heard: the sentence read with them.
        QVERIFY(QMetaObject::invokeMethod(desk.find("voicePreview"), "clicked"));
        QTRY_VERIFY(QString::fromUtf8(contents(log)).contains("-t female2"));
        QVERIFY(QString::fromUtf8(contents(log)).contains("This is how your books will be read aloud."));
        QTRY_COMPARE(text(desk, "voicePreview"), QString("Hear It"));
        desk.shot("ai-page-voice");

        // OpenAI's voices: a key, then hosted allowed, then its own voices, none chosen yet.
        desk.settings->setReadAloud([&] {
            QJsonObject setUp = desk.settings->readAloud();
            setUp.insert("openai_url", server.url());
            return setUp;
        }());
        pick(desk, "text-to-speechProvider", 4);
        QCOMPARE(desk.settings->readAloud().value("engine").toString(), QString("openai"));
        QTRY_COMPARE(text(desk, "text-to-speechSummary"), QString("Only a hosted service is set up, and hosted isn't allowed for it"));
        tick(desk, "text-to-speechHosted", true);
        QTRY_COMPARE(text(desk, "text-to-speechSummary"), QString("A key is needed"));
        QTRY_VERIFY(desk.shows("text-to-speechOpenAiKey"));
        QVERIFY(!desk.shows("text-to-speechAddress"));
        desk.find("text-to-speechOpenAiKey")->setProperty("text", "sk-voices");
        QVERIFY(QMetaObject::invokeMethod(desk.find("text-to-speechOpenAiKey"), "accepted"));
        QCOMPARE(AiEndpoints::keptKey(server.url(), m_secrets), QString("sk-voices"));
        QTRY_VERIFY2(text(desk, "text-to-speechSummary").startsWith("OpenAI · "), qPrintable(text(desk, "text-to-speechSummary")));
        QTRY_COMPARE(value(desk, "voiceChoice", "count").toInt(), 14);
        QCOMPARE(value(desk, "voiceChoice", "currentIndex").toInt(), 0);
        type(desk, "text-to-speechModel", "tts-1-hd");
        QCOMPARE(desk.settings->readAloud().value("openai_model").toString(), QString("tts-1-hd"));
        QVERIFY(!desk.settings->readAloud().contains("model"));
        QTRY_VERIFY(text(desk, "text-to-speechSummary").contains("tts-1-hd"));
        desk.shot("ai-page-voice-openai");
        // Back to speech-dispatcher: its voice and pace come back.
        pick(desk, "text-to-speechProvider", 2);
        QTRY_COMPARE(value(desk, "voiceChoice", "currentIndex").toInt(), 2);
        QCOMPARE(text(desk, "voiceRate"), QString("0.9×"));
        QVERIFY(AiEndpoints::keepKey(server.url(), {}, m_secrets));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));

        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk other(language, touch ? QByteArray() : lightTheme, touch);
            QVERIFY(fakeProviders(other, server));
            other.find("aiSettings")->setProperty("current", QString("text-to-speech"));
            pick(other, "text-to-speechProvider", 2);
            QTRY_VERIFY(other.shows("voiceChoice"));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(other.resize(size));
                other.settle();
                look(other, "ai-voice");
            }
            QVERIFY2(other.warnings.isEmpty(), qPrintable(other.warnings.join('\n')));
        }
        const QStringList problems = std::exchange(found, {});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }

    // The page at every size, in a long language, under fingers and right to
    // left, with each row open in turn: some ready, some not, one hosted.
    void thePageAtEverySize() {
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            desk.settings->setCapabilities(Capabilities::fresh());
            desk.settings->setAiCommand("cat");
            desk.settings->chooseAiProvider("command");
            desk.settings->setMeaning({{"url", "https://api.example.com/v1"}, {"model", "text-embedding-3-large"}});
            desk.settings->setReadAloud({{"engine", "server"}, {"url", "http://voice.example.com/v1"}});
            desk.aiAgent->capabilities()->watch(true);
            desk.aiAgent->capabilities()->allowHosted("embeddings", true);
            desk.aiAgent->capabilities()->watch(false);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                desk.quicken();
                desk.call("showSettings");
                QVERIFY(desk.waitForPopup());
                look(desk, "settings");
                QVERIFY(desk.closePopups());
                QVERIFY(open(desk));
                for (const char *row : {"", "answers", "quick", "handwriting", "speech-to-text", "text-to-speech", "embeddings"}) {
                    desk.find("aiSettings")->setProperty("current", QString(row));
                    desk.settle();
                    look(desk, QString("ai-") + (*row ? row : "closed"));
                }
                QVERIFY(desk.closePopups());
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = std::exchange(found, {});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }

    // Why, and what to do, in the person's language.
    void theReasonsAreWordedInTheirLanguage() {
        Desk desk("de", {}, true);
        desk.settings->setAiProvider("off");
        QVERIFY(desk.show());
        QVERIFY(open(desk));
        QTRY_VERIFY(!text(desk, "answersSummary").isEmpty());
        QVERIFY2(text(desk, "answersSummary") != QString("Off, as the AI is"), qPrintable(text(desk, "answersSummary")));
        QVERIFY(text(desk, "aiOffNote")
                != QString("Off: no model is asked anything, nothing is sent, and nothing is read for meaning, handwriting or speech. A "
                           "voice on this computer still reads aloud."));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(AiPageTests)
#include "aipage_tests.moc"
