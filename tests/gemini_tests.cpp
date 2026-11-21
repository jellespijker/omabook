// Gemini through its own API (#145): each capability it serves asked of a
// fake Gemini on this computer, the requests held to the shapes of Google's
// API documentation (gemini.h), the answers read as it writes them, its
// errors coming back as codes, and nothing sent where the person didn't allow
// a hosted service for that capability.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

#include "ai.h"
#include "airegistry.h"
#include "embeddings.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "gemini.h"
#include "inkstroke.h"
#include "recognition.h"
#include "secretstore.h"
#include "settings.h"
#include "voice.h"
#include "voices.h"

using namespace Qt::StringLiterals;

namespace {

// A response of generateContent with these parts.
[[nodiscard]] QJsonObject response(const QJsonArray &parts, const QString &finish = u"STOP"_s) {
    return {
        {"candidates", QJsonArray{QJsonObject{{"content", QJsonObject{{"role", "model"}, {"parts", parts}}}, {"finishReason", finish}}}},
        {"modelVersion", "gemini-4-hydrogen"}};
}

// The same as server-sent events, a piece per event, as streamGenerateContent?alt=sse writes them.
[[nodiscard]] QByteArray stream(const QList<QJsonObject> &events) {
    QByteArray body;
    for (const QJsonObject &event : events)
        body += "data: " + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\r\n\r\n";
    return body;
}

// Google's error body: {"error": {"code", "message", "status", "details"}}.
[[nodiscard]] QByteArray error(int code, const QString &status, const QString &message, const QString &reason = {}) {
    QJsonObject error{{"code", code}, {"message", message}, {"status", status}};
    if (!reason.isEmpty())
        error.insert("details", QJsonArray{QJsonObject{{"@type", "type.googleapis.com/google.rpc.ErrorInfo"}, {"reason", reason}}});
    return QJsonDocument(QJsonObject{{"error", error}}).toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::vector<Ink::Stroke> writing() {
    Ink::Stroke stroke{
        .id = Ink::newId(), .color = Qt::blue, .width = 3, .pressured = true, .tilted = false, .began = 1'700'000'000'000, .points = {}};
    for (int i = 0; i < 20; ++i)
        stroke.points.push_back({.at = {10.0, 10.0 + i * 4}, .pressure = 0.6F, .xTilt = 0, .yTilt = 0, .ms = static_cast<quint32>(i * 8)});
    return {stroke};
}

// Every capability's Gemini, as one asking would: answers' through its provider.
struct Answer {
    QString text;
    std::optional<Failure> failure;
    QList<Failure> notes;
};
[[nodiscard]] Answer ask(AiProvider &provider, const AiRequest &request) {
    Answer answer;
    const QMetaObject::Connection pieces =
        QObject::connect(&provider, &AiProvider::delta, [&answer](const QString &text) { answer.text += text; });
    const QMetaObject::Connection notes =
        QObject::connect(&provider, &AiProvider::noted, [&answer](const Failure &note) { answer.notes << note; });
    QSignalSpy finished(&provider, &AiProvider::finished);
    QSignalSpy failed(&provider, &AiProvider::failed);
    provider.ask(request);
    if (!QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 10000))
        answer.failure = Failure::said(u"no answer in time"_s);
    else if (!failed.isEmpty())
        answer.failure = failed.first().first().value<Failure>();
    QObject::disconnect(pieces);
    QObject::disconnect(notes);
    return answer;
}

// What one way of recognising made of the input, or why it couldn't.
struct Heard {
    std::optional<Recognition::Result> result;
    std::optional<Recognition::Failure> failure;
};
[[nodiscard]] Heard recognize(Recognition::Recognizer &way, const Recognition::Input &input) {
    Heard heard;
    const QMetaObject::Connection made = QObject::connect(&way, &Recognition::Recognizer::recognized,
                                                          [&heard](const Recognition::Result &result) { heard.result = result; });
    const QMetaObject::Connection lost = QObject::connect(&way, &Recognition::Recognizer::failed,
                                                          [&heard](const Recognition::Failure &failure) { heard.failure = failure; });
    way.recognize(input);
    if (!QTest::qWaitFor([&heard] { return heard.result || heard.failure; }, 10000))
        heard.failure = Recognition::Failure{.why = Recognition::Failure::Why::Away, .message = u"no answer in time"_s, .lacking = false};
    QObject::disconnect(made);
    QObject::disconnect(lost);
    return heard;
}

[[nodiscard]] bool near(const std::vector<float> &vector, std::initializer_list<float> expected) {
    return std::ranges::equal(vector, expected, [](float one, float other) { return std::abs(one - other) < 1e-6F; });
}

} // namespace

class GeminiTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;
    QTemporaryDir m_dir;

    // Settings with Gemini at the fake one's address, its key in the keyring,
    // and hosted allowed for every capability unless `hosted` is false.
    std::unique_ptr<Settings> settings(const FakeServer &server, bool hosted = true) {
        auto made = std::make_unique<Settings>(m_dir.filePath(u"config-%1.json"_s.arg(++m_files)));
        made->setGemini({{"url", server.url()}});
        QJsonObject all;
        for (const Capability capability : Capabilities::all) {
            QJsonObject one{{"hosted", hosted}};
            if (capability != Capability::TextToSpeech)
                one.insert("provider", "gemini");
            all.insert(Capabilities::name(capability), one);
        }
        made->setCapabilities(all);
        made->chooseAiProvider("gemini");
        made->setReadAloud({{"engine", "gemini"}});
        return made;
    }
    int m_files = 0;

private slots:
    void initTestCase() { Secrets::useInstead(&m_secrets); }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }
    void init() { QVERIFY(Gemini::keepKey(u"gemini-test-key"_s, m_secrets)); }

    // Answers stream as server-sent events, the conversation in Gemini's
    // turns, the key in its header and never in the address; its thinking is
    // never shown.
    void answersStreamInGeminisTurns() {
        FakeServer server;
        server.replies.append(
            {.status = "200 OK",
             .body = stream({response({QJsonObject{{"text", "Hel"}}}, {}),
                             response({QJsonObject{{"text", "working it out"}, {"thought", true}}, QJsonObject{{"text", "lo."}}})})});
        const auto chosen = settings(server);
        auto provider = AiRegistry::answers(*chosen);
        QVERIFY2(provider, qPrintable(Fixtures::whyNot(provider)));
        QCOMPARE((*provider)->name(), QString("Gemini"));
        AiRequest request{.system = "Be brief.",
                          .messages = {{"user", "Who is it?"}, {"assistant", "Whom?"}, {"user", "Ahab."}},
                          .tier = AiRequest::Main,
                          .model = {},
                          .prose = true};
        const Answer answer = ask(**provider, request);
        QVERIFY2(!answer.failure, qPrintable(answer.failure ? answer.failure->english() : QString()));
        QCOMPARE(answer.text, QString("Hello."));
        QCOMPARE(server.received.size(), 1);
        const FakeServer::Request &sent = server.received.first();
        QCOMPARE(sent.line, QByteArray("POST /v1/models/gemini-4-hydrogen:streamGenerateContent?alt=sse HTTP/1.1"));
        QVERIFY(sent.head.toLower().contains("x-goog-api-key: gemini-test-key"));
        QVERIFY(!sent.line.contains("key="));
        const QJsonObject body = sent.json();
        QCOMPARE(body.value("systemInstruction").toObject().value("parts").toArray().at(0).toObject().value("text").toString(),
                 QString("Be brief."));
        const QJsonArray contents = body.value("contents").toArray();
        QCOMPARE(contents.size(), 3);
        QCOMPARE(contents.at(1).toObject().value("role").toString(), QString("model"));
        QCOMPARE(contents.at(2).toObject().value("role").toString(), QString("user"));
        QCOMPARE(contents.at(2).toObject().value("parts").toArray().at(0).toObject().value("text").toString(), QString("Ahab."));

        // Quick work goes to the small model.
        server.replies.append({.status = "200 OK", .body = stream({response({QJsonObject{{"text", "ok"}}})})});
        request.tier = AiRequest::Quick;
        QCOMPARE(ask(**provider, request).text, QString("ok"));
        QVERIFY(server.received.last().line.contains("/models/gemini-3.5-flash-lite:streamGenerateContent"));
    }

    // What goes wrong comes back as a code (ADR 0005, #119).
    void itsErrorsAreCodes() {
        FakeServer server;
        const auto chosen = settings(server);
        auto provider = AiRegistry::answers(*chosen);
        QVERIFY2(provider, qPrintable(Fixtures::whyNot(provider)));
        const AiRequest request{.system = {}, .messages = {{"user", "Hi"}}, .tier = AiRequest::Main, .model = {}, .prose = false};

        server.replies.append(
            {.status = "400 Bad Request",
             .body = error(400, "INVALID_ARGUMENT", "API key not valid. Please pass a valid API key.", "API_KEY_INVALID")});
        QCOMPARE(ask(**provider, request).failure.value_or(Failure()), Failure(Failure::Code::GeminiRefusedKey));
        server.replies.append({.status = "429 Too Many Requests", .body = error(429, "RESOURCE_EXHAUSTED", "Quota exceeded.")});
        QCOMPARE(ask(**provider, request).failure.value_or(Failure()).code(), Failure::Code::AiLimited);
        // A prompt it blocks is declined, not an empty answer.
        server.replies.append(
            {.status = "200 OK", .body = stream({QJsonObject{{"promptFeedback", QJsonObject{{"blockReason", "SAFETY"}}}}})});
        QCOMPARE(ask(**provider, request).failure.value_or(Failure()), Failure(Failure::Code::AiDeclined));
        // A model it hasn't is asked once more with the main one, and said so.
        chosen->setAiModelFor("gemini", "quick", "gemini-0-none");
        server.replies.append(
            {.status = "404 Not Found", .body = error(404, "NOT_FOUND", "models/gemini-0-none is not found for API version v1beta.")});
        server.replies.append({.status = "200 OK", .body = stream({response({QJsonObject{{"text", "here"}}})})});
        auto again = AiRegistry::answers(*chosen);
        QVERIFY2(again, qPrintable(Fixtures::whyNot(again)));
        AiRequest quick = request;
        quick.tier = AiRequest::Quick;
        const Answer answer = ask(**again, quick);
        QCOMPARE(answer.text, QString("here"));
        QCOMPARE(answer.notes.size(), 1);
        QCOMPARE(answer.notes.first().code(), Failure::Code::AiQuickModelReplaced);
        QVERIFY(server.received.last().line.contains("/models/gemini-4-hydrogen:"));
        // An address that doesn't answer is the network's.
        Settings away(m_dir.filePath("away.json"));
        away.setCapabilities(chosen->capabilities());
        away.setGemini({{"url", "http://127.0.0.1:1/v1beta"}});
        away.chooseAiProvider("gemini");
        auto unreachable = AiRegistry::answers(away);
        QVERIFY2(unreachable, qPrintable(Fixtures::whyNot(unreachable)));
        QCOMPARE(ask(**unreachable, request).failure.value_or(Failure()).code(), Failure::Code::AiUnreachable);
    }

    // Gemini is someone else's computer for every capability: used only where
    // hosted is allowed for it, and only with a key.
    void itIsUsedOnlyWhereHostedIsAllowedAndWithAKey() {
        FakeServer server;
        const auto refused = settings(server, false);
        for (const Capability capability : Capabilities::all) {
            const CapabilityStatus status = AiRegistry::status(*refused, capability);
            QVERIFY2(!status.available, qPrintable(Capabilities::name(capability)));
            QCOMPARE(status.why, CapabilityLack::HostedNotAllowed);
        }
        const auto allowed = settings(server);
        for (const Capability capability : Capabilities::all) {
            const CapabilityStatus status = AiRegistry::status(*allowed, capability);
            QVERIFY2(status.available, qPrintable(Capabilities::name(capability) + u": "_s + status.failure.english()));
            QVERIFY(status.hosted);
            QCOMPARE(status.provider, QString("Gemini"));
        }
        QCOMPARE(AiRegistry::status(*allowed, Capability::Handwriting).model, QString("gemini-3.8-flash"));
        QCOMPARE(AiRegistry::status(*allowed, Capability::SpeechToText).model, QString("gemini-3.5-transcribe"));
        QCOMPARE(AiRegistry::status(*allowed, Capability::TextToSpeech).model, QString("gemini-3.8-flash-tts"));
        QCOMPARE(AiRegistry::status(*allowed, Capability::Embeddings).model, QString("gemini-embedding-001"));
        // A plugin that keeps to the person's own is never sent there.
        for (const Capability capability :
             {Capability::Handwriting, Capability::SpeechToText, Capability::TextToSpeech, Capability::Embeddings})
            QCOMPARE(AiRegistry::status(*allowed, capability, {.model = {}, .hosted = false}).why, CapabilityLack::HostedNotChosen);
        // Without a key, none of them.
        QVERIFY(Gemini::keepKey({}, m_secrets));
        qunsetenv(Gemini::keyVariable);
        const auto keyless = settings(server);
        for (const Capability capability : Capabilities::all)
            QCOMPARE(AiRegistry::status(*keyless, capability).why, CapabilityLack::NoKey);
        QCOMPARE(AiRegistry::status(*keyless, Capability::Answers).failure, Failure(Failure::Code::GeminiNoKey));
        // The AI off: nothing at all, a voice of Gemini's too.
        allowed->setAiProvider("off");
        for (const Capability capability : Capabilities::all)
            QCOMPARE(AiRegistry::status(*allowed, capability).why, CapabilityLack::AiOff);
        QVERIFY(server.received.isEmpty());
    }

    // Handwriting is shown to a model that reads pictures, inline as a PNG.
    void handwritingIsShownAsAPicture() {
        FakeServer server;
        server.answer(response({QJsonObject{{"text", "  Call me Ishmael.\n"}}}));
        const auto chosen = settings(server);
        auto ways = AiRegistry::recognizers(*chosen, Recognition::Kind::Ink, true);
        QVERIFY2(ways && ways->size() == 1, qPrintable(Fixtures::whyNot(ways)));
        const Heard heard = recognize(*ways->front(), {.kind = Recognition::Kind::Ink, .strokes = writing(), .file = {}, .language = "en"});
        QVERIFY2(heard.result, qPrintable(heard.failure ? heard.failure->message : QString()));
        const Recognition::Result &result = *heard.result;
        QCOMPARE(result.text, QString("Call me Ishmael."));
        QCOMPARE(result.server, QString("Gemini"));
        QCOMPARE(server.received.first().line, QByteArray("POST /v1/models/gemini-3.8-flash:generateContent HTTP/1.1"));
        const QJsonArray parts = server.received.first().json().value("contents").toArray().at(0).toObject().value("parts").toArray();
        QVERIFY(parts.at(0).toObject().value("text").toString().contains("handwriting"));
        const QJsonObject picture = parts.at(1).toObject().value("inlineData").toObject();
        QCOMPARE(picture.value("mimeType").toString(), QString("image/png"));
        QVERIFY(QByteArray::fromBase64(picture.value("data").toString().toLatin1()).startsWith("\x89PNG"));
        // Kept to one's own, it isn't asked at all.
        QVERIFY(!AiRegistry::recognizers(*chosen, Recognition::Kind::Ink, false));
    }

    // A recording is heard by the transcription model, each word with its moment.
    void speechIsWrittenOutWithEachWordsMoment() {
        FakeServer server;
        const QJsonArray words{QJsonObject{{"word", "Call"}, {"startOffset", "0.100s"}, {"endOffset", "0.400s"}},
                               QJsonObject{{"word", "me"}, {"startOffset", "0.400s"}, {"endOffset", "0.550s"}}};
        server.answer(
            response({QJsonObject{{"audioTranscription", QJsonObject{{"text", "Call me"}, {"speakerLabel", "spk_1"}, {"words", words}}}}}));
        const auto chosen = settings(server);
        const QString recording = m_dir.filePath("note.opus");
        QVERIFY(Fixtures::write(recording, "OggS-not-really"));
        auto ways = AiRegistry::recognizers(*chosen, Recognition::Kind::Audio, true);
        QVERIFY2(ways && ways->size() == 1, qPrintable(Fixtures::whyNot(ways)));
        QVERIFY(ways->front()->timesWords());
        const Heard heard =
            recognize(*ways->front(), {.kind = Recognition::Kind::Audio, .strokes = {}, .file = recording, .language = "en"});
        QVERIFY2(heard.result, qPrintable(heard.failure ? heard.failure->message : QString()));
        const Recognition::Result &result = *heard.result;
        QCOMPARE(result.text, QString("Call me"));
        QVERIFY(result.timed);
        QCOMPARE(result.segments.size(), 1);
        QCOMPARE(result.segments.first().words.size(), 2);
        QCOMPARE(result.segments.first().words.at(1).start, 400);
        QCOMPARE(result.segments.first().end, 550);
        QCOMPARE(server.received.first().line, QByteArray("POST /v1/models/gemini-3.5-transcribe:generateContent HTTP/1.1"));
        const QJsonObject body = server.received.first().json();
        const QJsonObject how = body.value("generationConfig").toObject().value("audioTranscriptionConfig").toObject();
        QVERIFY(how.value("wordTimestamp").toBool());
        QCOMPARE(how.value("languageCodes").toArray().at(0).toString(), QString("en"));
        const QJsonObject audio =
            body.value("contents").toArray().at(0).toObject().value("parts").toArray().at(0).toObject().value("inlineData").toObject();
        QCOMPARE(audio.value("mimeType").toString(), QString("audio/ogg"));
        QCOMPARE(QByteArray::fromBase64(audio.value("data").toString().toLatin1()), QByteArray("OggS-not-really"));

        // Another model that hears is asked in words, and gives only the words.
        QJsonObject gemini = chosen->gemini();
        gemini.insert("models", QJsonObject{{"speech-to-text", "gemini-3.8-flash"}});
        chosen->setGemini(gemini);
        server.answer(response({QJsonObject{{"text", "Call me Ishmael."}}}));
        auto plain = AiRegistry::recognizers(*chosen, Recognition::Kind::Audio, true);
        QVERIFY(plain && !plain->front()->timesWords());
        const Heard plainly =
            recognize(*plain->front(), {.kind = Recognition::Kind::Audio, .strokes = {}, .file = recording, .language = {}});
        QVERIFY2(plainly.result, qPrintable(plainly.failure ? plainly.failure->message : QString()));
        QCOMPARE(plainly.result->text, QString("Call me Ishmael."));
        QVERIFY(!plainly.result->timed);
        const QJsonArray asked = server.received.last().json().value("contents").toArray().at(0).toObject().value("parts").toArray();
        QVERIFY(asked.at(0).toObject().value("text").toString().startsWith("Generate a transcript"));
        QVERIFY(!server.received.last().json().contains("generationConfig"));

        // Too long to send at once: said, and nothing sent.
        const QString longer = m_dir.filePath("long.wav");
        QFile file(longer);
        QVERIFY(file.open(QIODevice::WriteOnly) && file.resize(Gemini::largestRecording + 1));
        file.close();
        const qsizetype before = server.received.size();
        const Heard tooLong = recognize(*plain->front(), {.kind = Recognition::Kind::Audio, .strokes = {}, .file = longer, .language = {}});
        QVERIFY(tooLong.failure && tooLong.failure->message.contains("too long"));
        QCOMPARE(server.received.size(), before);
    }

    // The meaning index: a batch of texts, each with its task, in their order.
    void embeddingsComeInABatch() {
        FakeServer server;
        server.answer(
            {{"embeddings", QJsonArray{QJsonObject{{"values", QJsonArray{3.0, 4.0}}}, QJsonObject{{"values", QJsonArray{0.0, 2.0}}}}}});
        const auto chosen = settings(server);
        const auto route = AiRegistry::embeddings(*chosen);
        QVERIFY2(route, qPrintable(Fixtures::whyNot(route)));
        QVERIFY(route->hosted);
        const auto vectors = Embeddings::embed(*route, {"whale", "sea"}, Embeddings::Use::Question);
        QVERIFY2(vectors, qPrintable(Fixtures::whyNot(vectors)));
        QCOMPARE(vectors->size(), std::size_t{2});
        QVERIFY(near(vectors->at(0), {0.6F, 0.8F}));
        QVERIFY(near(vectors->at(1), {0.0F, 1.0F}));
        QCOMPARE(server.received.first().line, QByteArray("POST /v1/models/gemini-embedding-001:batchEmbedContents HTTP/1.1"));
        const QJsonArray requests = server.received.first().json().value("requests").toArray();
        QCOMPARE(requests.size(), 2);
        const QJsonObject first = requests.at(0).toObject();
        QCOMPARE(first.value("model").toString(), QString("models/gemini-embedding-001"));
        QCOMPARE(first.value("content").toObject().value("parts").toArray().at(0).toObject().value("text").toString(), QString("whale"));
        QCOMPARE(first.value("embedContentConfig").toObject().value("taskType").toString(), QString("RETRIEVAL_QUERY"));
        // A book's text is a document, and a model that takes no task is given none.
        const QJsonObject document = Gemini::embedBody("gemini-embedding-2", {"Call me Ishmael."}, Gemini::Use::Document);
        QVERIFY(!document.value("requests").toArray().at(0).toObject().value("embedContentConfig").toObject().contains("taskType"));
        QCOMPARE(Gemini::embedBody("gemini-embedding-001", {"x"}, Gemini::Use::Document)
                     .value("requests")
                     .toArray()
                     .at(0)
                     .toObject()
                     .value("embedContentConfig")
                     .toObject()
                     .value("taskType")
                     .toString(),
                 QString("RETRIEVAL_DOCUMENT"));
        // An answer short of vectors is said so.
        server.answer({{"embeddings", QJsonArray{QJsonObject{{"values", QJsonArray{1.0}}}}}});
        const auto short_ = Embeddings::embed(*route, {"a", "b"}, Embeddings::Use::Document);
        QVERIFY(!short_);
        QCOMPARE(short_.error().code(), Failure::Code::MeaningNoVectors);
    }

    // Reading aloud: a sentence in one of Gemini's voices, answered as sound.
    void aVoiceOfGeminisReadsASentence() {
        FakeServer server;
        const QByteArray pcm(4800, '\0');
        server.answer(response({QJsonObject{
            {"inlineData", QJsonObject{{"mimeType", "audio/L16;codec=pcm;rate=24000"}, {"data", QString::fromLatin1(pcm.toBase64())}}}}}));
        const auto chosen = settings(server);
        auto voice = AiRegistry::voice(*chosen);
        QVERIFY2(voice, qPrintable(Fixtures::whyNot(voice)));
        QCOMPARE((*voice)->name(), QString("Gemini"));
        QCOMPARE(Voices::offered(*chosen).first(), QString("Kore"));
        (*voice)->say("Call me Ishmael.", {.rate = 1.0, .voice = "Puck", .language = "en"});
        QTRY_COMPARE(server.received.size(), 1);
        QCOMPARE(server.received.first().line, QByteArray("POST /v1/models/gemini-3.8-flash-tts:generateContent HTTP/1.1"));
        const QJsonObject body = server.received.first().json();
        const QJsonObject config = body.value("generationConfig").toObject();
        QCOMPARE(config.value("responseModalities").toArray(), QJsonArray{"AUDIO"});
        QCOMPARE(config.value("speechConfig")
                     .toObject()
                     .value("voiceConfig")
                     .toObject()
                     .value("prebuiltVoiceConfig")
                     .toObject()
                     .value("voiceName")
                     .toString(),
                 QString("Puck"));
        QCOMPARE(body.value("contents").toArray().at(0).toObject().value("parts").toArray().at(0).toObject().value("text").toString(),
                 QString("Call me Ishmael."));
        // A voice it hasn't is its own.
        QCOMPARE(Gemini::speechBody("Hi.", "alloy")
                     .value("generationConfig")
                     .toObject()
                     .value("speechConfig")
                     .toObject()
                     .value("voiceConfig")
                     .toObject()
                     .value("prebuiltVoiceConfig")
                     .toObject()
                     .value("voiceName")
                     .toString(),
                 QString("Kore"));

        // Raw PCM becomes a WAV at its rate; a WAV stays as it is.
        const QByteArray sound = Gemini::soundOf(response({QJsonObject{
            {"inlineData", QJsonObject{{"mimeType", "audio/L16;codec=pcm;rate=16000"}, {"data", QString::fromLatin1(pcm.toBase64())}}}}}));
        QCOMPARE(sound.size(), pcm.size() + 44);
        QVERIFY(sound.startsWith("RIFF") && sound.mid(8, 4) == "WAVE");
        QCOMPARE(qFromLittleEndian<quint32>(sound.constData() + 24), 16000U);
        const QByteArray riff = "RIFF....WAVEfmt ";
        QCOMPARE(Gemini::soundOf(response({QJsonObject{
                     {"inlineData", QJsonObject{{"mimeType", "audio/wav"}, {"data", QString::fromLatin1(riff.toBase64())}}}}})),
                 riff);

        // A sentence answered without sound, or a key refused, is said so.
        server.answer(response({QJsonObject{{"text", "no"}}}));
        QSignalSpy failed(voice->get(), &Voice::failed);
        (*voice)->say("Again.", {});
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().value<Failure>(), Failure(Failure::Code::GeminiNoSound));
        server.replies.append(
            {.status = "400 Bad Request", .body = error(400, "INVALID_ARGUMENT", "API key not valid.", "API_KEY_INVALID")});
        (*voice)->say("Again.", {});
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(failed.last().first().value<Failure>(), Failure(Failure::Code::GeminiRefusedKey));
    }

    // The key: in the keyring, never in config.json.
    void theKeyIsKeptInTheKeyring() {
        FakeServer server;
        const auto chosen = settings(server);
        QVERIFY(Gemini::keepKey(u" another-key "_s, m_secrets));
        QCOMPARE(m_secrets.get("gemini"), QString("another-key"));
        QCOMPARE(Gemini::api(*chosen)->key, QString("another-key"));
        QFile file(m_dir.filePath(u"config-%1.json"_s.arg(m_files)));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("another-key"));
        QVERIFY(Gemini::keepKey({}, m_secrets));
        QVERIFY(!Gemini::hasKey());
    }
};

QTEST_MAIN(GeminiTests)
#include "gemini_tests.moc"
