// OpenAI's voices over its Realtime API (#169), against a WebSocket server on
// this computer that speaks the protocol's shapes (fakerealtimeserver.h):
// what a sentence is sent as, the sound it answers, a session kept while the
// voice and pace stay, stopping, its errors as codes, and OpenAI's voices
// going on to the successor once /audio/speech has no model left, with the
// privacy rule and AI Off as before.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QtEndian>
#include <QtTest>
#include <memory>

#include "aiendpoint.h"
#include "airegistry.h"
#include "fakeaiserver.h"
#include "fakerealtimeserver.h"
#include "fixtures.h"
#include "localvoices.h"
#include "openaivoice.h"
#include "realtimevoice.h"
#include "secretstore.h"
#include "servervoice.h"
#include "settings.h"
#include "voicepreview.h"
#include "voices.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] AiServer openAi(const QString &base) {
    return {.preset = u"openai"_s, .name = u"OpenAI"_s, .baseUrl = base, .key = u"sk-realtime"_s, .keyFile = {}, .model = {}};
}

// Plays nothing: keeps what it was given and ends at once, as a sound would.
class FakeSoundOut : public SoundOut {
public:
    explicit FakeSoundOut(QList<QByteArray> &played) : m_played(played) {}
    void play(const QString &file) override {
        QFile sound(file);
        m_played.append(sound.open(QIODevice::ReadOnly) ? sound.readAll() : QByteArray());
        QTimer::singleShot(0, this, &SoundOut::ended);
    }
    void stop() override {}

private:
    QList<QByteArray> &m_played;
};

// A WAV of mono 16-bit samples at 24 kHz holding `pcm`.
[[nodiscard]] bool wavOf(const QByteArray &wav, const QByteArray &pcm) {
    return wav.startsWith("RIFF") && wav.mid(8, 4) == "WAVE" && qFromLittleEndian<quint16>(wav.constData() + 22) == 1
           && qFromLittleEndian<quint32>(wav.constData() + 24) == 24000 && wav.endsWith(pcm) && wav.size() == pcm.size() + 44;
}

// What /audio/speech answers for a model OpenAI has shut down.
[[nodiscard]] FakeServer::Reply retired() {
    return {
        .status = "404 Not Found",
        .body =
            R"({"error":{"message":"The model `gpt-4o-mini-tts` has been deprecated and shut down.","type":"invalid_request_error","code":"model_not_found"}})"};
}

} // namespace

class RealtimeVoiceTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    SessionSecrets m_secrets;
    int m_files = 0;

    [[nodiscard]] std::unique_ptr<Settings> settings(const QJsonObject &readAloud, bool hosted = true) {
        auto made = std::make_unique<Settings>(m_dir.filePath(u"config-%1.json"_s.arg(++m_files)));
        made->setCapabilities({{"text-to-speech", QJsonObject{{"hosted", hosted}}}});
        made->setReadAloud(readAloud);
        return made;
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<Failure>();
        Secrets::useInstead(&m_secrets);
        qunsetenv("OPENAI_API_KEY");
    }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    // A sentence: the session told how to read it, the sentence asked out of
    // band, and the sound it answers played.
    void aSentenceIsReadOverTheRealtimeApi() {
        FakeRealtimeServer server;
        QList<QByteArray> played;
        RealtimeVoice voice(openAi(u"https://api.openai.com/v1"_s), QUrl(server.url()), QString::fromLatin1(RealtimeVoice::openAiModel),
                            std::make_unique<FakeSoundOut>(played));
        QSignalSpy spoken(&voice, &Voice::spoken);
        QSignalSpy failed(&voice, &Voice::failed);
        voice.say(u"Call me Ishmael."_s, {.rate = 2.0, .voice = u"coral"_s, .language = u"en"_s});
        QTRY_COMPARE(spoken.size(), 1);
        QVERIFY(failed.isEmpty());
        QCOMPARE(played.size(), 1);
        QVERIFY(wavOf(played.first(), FakeRealtimeServer::pcm()));
        QCOMPARE(server.connections.size(), 1);
        const QNetworkRequest &opened = server.connections.first().request;
        QCOMPARE(opened.url().path(), u"/v1/realtime"_s);
        QCOMPARE(QUrlQuery(opened.url()).queryItemValue(u"model"_s), QString::fromLatin1(RealtimeVoice::openAiModel));
        QCOMPARE(opened.rawHeader("Authorization"), QByteArray("Bearer sk-realtime"));

        const QList<QJsonObject> sessions = server.sentOf(0, u"session.update"_s);
        QCOMPARE(sessions.size(), 1);
        const QJsonObject session = sessions.first().value("session").toObject();
        QCOMPARE(session.value("type").toString(), u"realtime"_s);
        QCOMPARE(session.value("output_modalities").toArray(), QJsonArray{u"audio"_s});
        QVERIFY(session.value("instructions").toString().contains(u"exactly as it is written"_s));
        const QJsonObject output = session.value("audio").toObject().value("output").toObject();
        QCOMPARE(output.value("voice").toString(), u"coral"_s);
        QCOMPARE(output.value("speed").toDouble(), 1.5); // its fastest
        QCOMPARE(output.value("format").toObject(), (QJsonObject{{"type", "audio/pcm"}, {"rate", 24000}}));

        const QList<QJsonObject> asked = server.sentOf(0, u"response.create"_s);
        QCOMPARE(asked.size(), 1);
        const QJsonObject response = asked.first().value("response").toObject();
        QCOMPARE(response.value("conversation").toString(), u"none"_s);
        const QJsonObject message = response.value("input").toArray().first().toObject();
        QCOMPARE(message.value("role").toString(), u"user"_s);
        QCOMPARE(message.value("content").toArray().first().toObject(),
                 (QJsonObject{{"type", "input_text"}, {"text", "Call me Ishmael."}}));
        // Not one of its voices: its own default.
        QCOMPARE(RealtimeVoice::session({.rate = 1.0, .voice = u"nova"_s, .language = {}})
                     .value("session")
                     .toObject()
                     .value("audio")
                     .toObject()
                     .value("output")
                     .toObject()
                     .value("voice")
                     .toString(),
                 QString::fromLatin1(RealtimeVoice::defaultVoice));
    }

    // The next sentence in the same voice and pace goes over the same
    // session; another voice can't be had there, so it opens one of its own.
    void aSessionIsKeptWhileTheVoiceStays() {
        FakeRealtimeServer server;
        QList<QByteArray> played;
        RealtimeVoice voice(openAi(u"https://api.openai.com/v1"_s), QUrl(server.url()), {}, std::make_unique<FakeSoundOut>(played));
        QSignalSpy spoken(&voice, &Voice::spoken);
        const Speech how{.rate = 1.0, .voice = u"marin"_s, .language = u"en"_s};
        voice.say(u"One."_s, how);
        QTRY_COMPARE(spoken.size(), 1);
        voice.say(u"Two."_s, how);
        QTRY_COMPARE(spoken.size(), 2);
        QCOMPARE(server.connections.size(), 1);
        QCOMPARE(server.sentOf(0, u"session.update"_s).size(), 1);
        QCOMPARE(server.sentOf(0, u"response.create"_s).size(), 2);
        voice.say(u"Three."_s, {.rate = 1.0, .voice = u"cedar"_s, .language = u"en"_s});
        QTRY_COMPARE(spoken.size(), 3);
        QCOMPARE(played.size(), 3);
        QCOMPARE(server.connections.size(), 2);
        QCOMPARE(server.sentOf(1, u"session.update"_s)
                     .first()
                     .value("session")
                     .toObject()
                     .value("audio")
                     .toObject()
                     .value("output")
                     .toObject()
                     .value("voice")
                     .toString(),
                 u"cedar"_s);
    }

    // Stopped while it is being made: the session goes, and nothing more is said of it.
    void stoppingLetsTheSentenceGo() {
        FakeRealtimeServer server;
        server.answering = [](const QJsonObject &) { return QList<QJsonObject>{}; };
        QList<QByteArray> played;
        RealtimeVoice voice(openAi(u"https://api.openai.com/v1"_s), QUrl(server.url()), {}, std::make_unique<FakeSoundOut>(played));
        QSignalSpy spoken(&voice, &Voice::spoken);
        QSignalSpy failed(&voice, &Voice::failed);
        voice.say(u"Call me Ishmael."_s, {});
        QTRY_COMPARE(server.sentOf(0, u"response.create"_s).size(), 1);
        voice.stop();
        QTRY_VERIFY(server.connections.first().closed);
        QTest::qWait(50);
        QVERIFY(spoken.isEmpty());
        QVERIFY(failed.isEmpty());
        QVERIFY(played.isEmpty());
    }

    // What goes wrong is a code, worded where it is shown.
    void itsErrorsAreCodes_data() {
        QTest::addColumn<QList<QJsonObject>>("answer");
        QTest::addColumn<Failure>("expected");
        QTest::newRow("refused key") << QList<QJsonObject>{FakeRealtimeServer::error(u"invalid_request_error"_s, u"invalid_api_key"_s,
                                                                                     u"Incorrect API key provided."_s)}
                                     << Failure(Failure::Code::VoiceKeyRefused, {u"OpenAI"_s});
        QTest::newRow("model gone") << QList<QJsonObject>{FakeRealtimeServer::error(u"invalid_request_error"_s, u"model_not_found"_s,
                                                                                    u"The model does not exist."_s)}
                                    << Failure(Failure::Code::VoiceModelRetired, {u"OpenAI"_s, u"gpt-realtime-2.1-mini"_s});
        QTest::newRow("failed") << QList<QJsonObject>{FakeRealtimeServer::done(
            u"failed"_s, {{"type", "failed"}, {"error", QJsonObject{{"type", "server_error"}, {"message", "Overloaded."}}}})}
                                << Failure(Failure::Code::VoiceServerFailed, {u"OpenAI"_s, u"Overloaded."_s});
        QTest::newRow("filtered") << QList<QJsonObject>{FakeRealtimeServer::done(u"incomplete"_s,
                                                                                 {{"type", "incomplete"}, {"reason", "content_filter"}})}
                                  << Failure(Failure::Code::VoiceServerFailed, {u"OpenAI"_s, u"content_filter"_s});
        QTest::newRow("no sound") << QList<QJsonObject>{FakeRealtimeServer::done(u"completed"_s)}
                                  << Failure(Failure::Code::VoiceNoSound, {u"OpenAI"_s});
    }
    void itsErrorsAreCodes() {
        QFETCH(QList<QJsonObject>, answer);
        QFETCH(Failure, expected);
        FakeRealtimeServer server;
        server.answering = [answer](const QJsonObject &) { return answer; };
        QList<QByteArray> played;
        RealtimeVoice voice(openAi(u"https://api.openai.com/v1"_s), QUrl(server.url()), {}, std::make_unique<FakeSoundOut>(played));
        QSignalSpy failed(&voice, &Voice::failed);
        voice.say(u"Call me Ishmael."_s, {});
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().value<Failure>(), expected);
        QVERIFY(!expected.text().isEmpty());
        QVERIFY(played.isEmpty());
        // A server that isn't there says so too.
        RealtimeVoice away(openAi(u"https://api.openai.com/v1"_s), QUrl(u"ws://127.0.0.1:1/v1/realtime"_s), {},
                           std::make_unique<FakeSoundOut>(played));
        QSignalSpy unreached(&away, &Voice::failed);
        away.say(u"Hi."_s, {});
        QTRY_COMPARE(unreached.size(), 1);
        QCOMPARE(unreached.first().first().value<Failure>().code(), Failure::Code::VoiceServerFailed);
    }

    // Where its Realtime API is, by the API's address.
    void itsAddressIsTheApisOverAWebSocket() {
        QCOMPARE(RealtimeVoice::addressOf(u"https://api.openai.com/v1"_s), QUrl(u"wss://api.openai.com/v1/realtime"_s));
        QCOMPARE(RealtimeVoice::addressOf(u"http://127.0.0.1:8080/v1"_s), QUrl(u"ws://127.0.0.1:8080/v1/realtime"_s));
        QVERIFY(RealtimeVoice::realtimeOnly(u"gpt-realtime-2.1-mini"_s));
        QVERIFY(!RealtimeVoice::realtimeOnly(u"gpt-4o-mini-tts"_s));
    }

    // OpenAI's voices with a model of the Realtime API's: read there, with
    // its voices, by the same rules as /audio/speech's.
    void openAisVoicesTakeARealtimeModelWhereAllowed() {
        FakeServer speech;
        FakeRealtimeServer realtime;
        const QJsonObject setUp{{"engine", "openai"},
                                {"openai_url", speech.url()},
                                {"openai_realtime_url", realtime.url()},
                                {"openai_model", "gpt-realtime-2.1-mini"}};
        QVERIFY(AiEndpoints::keepKey(speech.url(), "sk-voices", m_secrets));
        auto refused = settings(setUp, false);
        QCOMPARE(Voices::engine(*refused).error(), Failure(Failure::Code::VoiceServerHosted, {"OpenAI"}));
        auto chosen = settings(setUp);
        QCOMPARE(Voices::offered(*chosen), RealtimeVoice::voices());
        QCOMPARE(AiRegistry::status(*chosen, Capability::TextToSpeech).model, u"gpt-realtime-2.1-mini"_s);
        auto voice = AiRegistry::voice(*chosen);
        QVERIFY2(voice, qPrintable(Fixtures::whyNot(voice)));
        (*voice)->say(u"Call me Ishmael."_s, Voices::speech(*chosen, "en"));
        QTRY_COMPARE(realtime.connections.size(), 1);
        QTRY_COMPARE(realtime.sentOf(0, u"response.create"_s).size(), 1);
        (*voice)->stop();
        QVERIFY(speech.received.isEmpty());
        QCOMPARE(realtime.connections.first().request.rawHeader("Authorization"), QByteArray("Bearer sk-voices"));
        chosen->setAiProvider("off");
        QCOMPARE(Voices::engine(*chosen).error().code(), Failure::Code::VoiceAiOff);
        QVERIFY(AiEndpoints::keepKey(speech.url(), {}, m_secrets));
    }

    // /audio/speech shut down (2027-01-06): the sentence is read by the
    // successor over the Realtime API, which is kept, so the next voice
    // goes there at once; the preview ("Hear It") does the same.
    void openAisVoicesGoOnToTheSuccessor() {
        FakeServer speech;
        speech.answering = [](const FakeServer::Request &) { return retired(); };
        FakeRealtimeServer realtime;
        QVERIFY(AiEndpoints::keepKey(speech.url(), "sk-voices", m_secrets));
        auto chosen = settings({{"engine", "openai"}, {"openai_url", speech.url()}, {"openai_realtime_url", realtime.url()}});
        VoicePreview preview(chosen.get());
        preview.play(u"This is how your books will be read aloud."_s, u"en"_s);
        QTRY_COMPARE(realtime.connections.size(), 1);
        QTRY_COMPARE(realtime.sentOf(0, u"response.create"_s).size(), 1);
        QTest::qWait(50);
        // Read by the successor (whether this computer can play it is no matter here).
        QVERIFY2(preview.why().isEmpty() || !preview.why().contains(u"retired"_s), qPrintable(preview.why()));
        preview.stop();
        QCOMPARE(speech.received.size(), 1);
        QCOMPARE(speech.received.first().json().value("model").toString(), QString::fromLatin1(ServerVoice::openAiModel));
        QCOMPARE(QUrlQuery(realtime.connections.first().request.url()).queryItemValue(u"model"_s),
                 QString::fromLatin1(RealtimeVoice::openAiModel));
        QCOMPARE(realtime.sentOf(0, u"response.create"_s)
                     .first()
                     .value("response")
                     .toObject()
                     .value("input")
                     .toArray()
                     .first()
                     .toObject()
                     .value("content")
                     .toArray()
                     .first()
                     .toObject()
                     .value("text")
                     .toString(),
                 u"This is how your books will be read aloud."_s);
        // Kept: what reads next is the successor, and /audio/speech isn't asked again.
        QCOMPARE(Voices::openAiDefault(*chosen), QString::fromLatin1(RealtimeVoice::openAiModel));
        QCOMPARE(Voices::offered(*chosen), RealtimeVoice::voices());
        auto voice = AiRegistry::voice(*chosen);
        QVERIFY2(voice, qPrintable(Fixtures::whyNot(voice)));
        (*voice)->say(u"Call me Ishmael."_s, Voices::speech(*chosen, "en"));
        QTRY_COMPARE(realtime.connections.size(), 2);
        QTRY_COMPARE(realtime.sentOf(1, u"response.create"_s).size(), 1);
        (*voice)->stop();
        QCOMPARE(speech.received.size(), 1);
        QVERIFY(AiEndpoints::keepKey(speech.url(), {}, m_secrets));
    }
};

QTEST_MAIN(RealtimeVoiceTests)
#include "realtimevoice_tests.moc"
