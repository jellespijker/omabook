// The voices text to speech chooses from (#147, ADR 0008), each against a
// fake: Piper and speech-dispatcher as scripts on the path, a server of one's
// own, OpenAI's and Gemini's voices as a server on this computer at their
// address. What each is asked, which may read (hosted ones only where hosted
// is allowed, never with the AI off; a local voice reads with it off too), a
// voice and a pace kept per provider, and the AI page's preview.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <memory>

#include "aiendpoint.h"
#include "airegistry.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "gemini.h"
#include "secretstore.h"
#include "servervoice.h"
#include "settings.h"
#include "voicepreview.h"
#include "voices.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool program(const QString &path, const QByteArray &script) {
    return Fixtures::write(path, script) && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

[[nodiscard]] QString contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// A WAV of a moment of silence, as a server answers.
[[nodiscard]] QByteArray silence() {
    QByteArray wav("RIFF");
    const QByteArray pcm(1600, '\0');
    const auto le32 = [](quint32 value) {
        QByteArray bytes(4, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    const auto le16 = [](quint16 value) {
        QByteArray bytes(2, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    wav += le32(36 + pcm.size()) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(16000) + le32(32000) + le16(2) + le16(16);
    return wav + "data" + le32(pcm.size()) + pcm;
}

// Says a sentence and waits for it to be asked of the server.
void sayTo(Voice &voice, const FakeServer &server, const Speech &how) {
    voice.say(u"Call me Ishmael."_s, how);
    QTRY_COMPARE(server.received.size(), 1);
    voice.stop();
}

} // namespace

class VoicesTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    SessionSecrets m_secrets;
    QByteArray m_path;
    int m_files = 0;

    [[nodiscard]] QString bin(const QString &name) const { return m_dir.filePath("bin/" + name); }
    [[nodiscard]] QString log() const { return m_dir.filePath("said.log"); }
    [[nodiscard]] std::unique_ptr<Settings> settings(const QJsonObject &readAloud, bool hosted = false) {
        auto made = std::make_unique<Settings>(m_dir.filePath(u"config-%1.json"_s.arg(++m_files)));
        made->setCapabilities({{"text-to-speech", QJsonObject{{"hosted", hosted}}}});
        made->setReadAloud(readAloud);
        return made;
    }

private slots:
    void initTestCase() {
        Secrets::useInstead(&m_secrets);
        m_path = qgetenv("PATH");
        // Piper and speech-dispatcher, writing down what they are asked; the
        // one reads its sentence from its input and writes a WAV, the other
        // says it and returns (or is told to stop, `-S`).
        QVERIFY(QDir().mkpath(m_dir.filePath("bin")) && QDir().mkpath(m_dir.filePath("share/piper-voices")));
        QVERIFY(program(bin("piper"), "#!/bin/bash\necho \"piper $*\" >> '" + log().toUtf8() + "'\ncat >> '" + log().toUtf8()
                                          + "'\necho >> '" + log().toUtf8() + "'\nprintf RIFF > \"$4\"\n"));
        QVERIFY(program(bin("spd-say"), "#!/bin/bash\necho \"spd-say $*\" >> '" + log().toUtf8() + "'\n"));
        QVERIFY(Fixtures::write(m_dir.filePath("share/piper-voices/en_US-fake-medium.onnx"), "model"));
        qputenv("PATH", m_dir.filePath("bin").toUtf8() + ":/usr/bin:/bin");
        qputenv("XDG_DATA_DIRS", m_dir.filePath("share").toUtf8());
        qunsetenv("OPENAI_API_KEY");
        qunsetenv("GEMINI_API_KEY");
    }
    void cleanupTestCase() {
        qputenv("PATH", m_path);
        Secrets::useInstead(nullptr);
    }
    void init() { QFile::remove(log()); }

    // Piper is given the voice and the pace, and the sentence on its input.
    void piperReadsInItsVoiceAtItsPace() {
        const auto chosen = settings({{"engine", "piper"}, {"piper_program", bin("piper")}});
        Voices::keep(*chosen, "voice", "en_US-fake-medium");
        Voices::keep(*chosen, "rate", 2.0);
        auto voice = AiRegistry::voice(*chosen);
        QVERIFY2(voice, qPrintable(Fixtures::whyNot(voice)));
        QCOMPARE((*voice)->name(), QString("Piper"));
        QVERIFY(Voices::offered(*chosen).contains("en_US-fake-medium"));
        (*voice)->say("Call me Ishmael.", Voices::speech(*chosen, "en"));
        QTRY_VERIFY(contents(log()).contains("Call me Ishmael."));
        QVERIFY2(contents(log()).contains(m_dir.filePath("share/piper-voices/en_US-fake-medium.onnx")), qPrintable(contents(log())));
        QVERIFY(contents(log()).contains("--length_scale 0.500"));
        (*voice)->stop();
    }
    // speech-dispatcher is given its kind of voice and its rate, and is done once it returns.
    void speechDispatcherReadsInItsVoiceAtItsPace() {
        const auto chosen = settings({{"engine", "speech-dispatcher"}});
        Voices::keep(*chosen, "voice", "female2");
        Voices::keep(*chosen, "rate", 1.5);
        auto voice = AiRegistry::voice(*chosen);
        QVERIFY2(voice, qPrintable(Fixtures::whyNot(voice)));
        QSignalSpy spoken(voice->get(), &Voice::spoken);
        (*voice)->say("Call me Ishmael.", Voices::speech(*chosen, "en-GB"));
        QTRY_COMPARE(spoken.size(), 1);
        QCOMPARE(contents(log()).trimmed(), QString("spd-say -w -r 50 -t female2 -l en -- Call me Ishmael."));
        // Automatic takes it where Piper has no voice.
        const auto automatic = settings({{"piper_program", "no-such-piper"}});
        QCOMPARE(Voices::engine(*automatic).value_or("none"), QString("speech-dispatcher"));
    }
    // A server of one's own, as OpenAI's API has it.
    void aServerOfOnesOwnReadsWithItsModelAndVoice() {
        FakeServer server;
        server.replies.append({.status = "200 OK", .body = silence()});
        const auto chosen = settings({{"engine", "server"}, {"url", server.url()}, {"model", "kokoro"}});
        Voices::keep(*chosen, "voice", "nova");
        auto voice = AiRegistry::voice(*chosen);
        QVERIFY2(voice, qPrintable(Fixtures::whyNot(voice)));
        QCOMPARE((*voice)->name(), QString("127.0.0.1:%1").arg(server.serverPort()));
        QCOMPARE(AiRegistry::status(*chosen, Capability::TextToSpeech).hosted, false);
        sayTo(**voice, server, Voices::speech(*chosen, "en"));
        QCOMPARE(server.received.first().line, QByteArray("POST /v1/audio/speech HTTP/1.1"));
        const QJsonObject asked = server.received.first().json();
        QCOMPARE(asked.value("model").toString(), QString("kokoro"));
        QCOMPARE(asked.value("voice").toString(), QString("nova"));
        QCOMPARE(asked.value("input").toString(), QString("Call me Ishmael."));
    }
    // OpenAI's voices: hosted, so only where allowed and never with the AI
    // off, with its key, its model and its voices.
    void openAisVoicesReadOnlyWhereAllowedWithItsKey() {
        FakeServer server;
        server.replies.append({.status = "200 OK", .body = silence()});
        const QJsonObject setUp{{"engine", "openai"}, {"openai_url", server.url()}};
        auto chosen = settings(setUp);
        QCOMPARE(Voices::engine(*chosen).error(), Failure(Failure::Code::VoiceServerHosted, {"OpenAI"}));
        QCOMPARE(AiRegistry::status(*chosen, Capability::TextToSpeech).why, CapabilityLack::HostedNotAllowed);
        chosen = settings(setUp, true);
        QCOMPARE(Voices::engine(*chosen).error(), Failure(Failure::Code::VoiceOpenAiNoKey));
        QCOMPARE(AiRegistry::status(*chosen, Capability::TextToSpeech).why, CapabilityLack::NoKey);
        QVERIFY(AiEndpoints::keepKey(server.url(), "sk-voices", m_secrets));
        QVERIFY(Voices::openAiHasKey(*chosen));
        // A plugin asks for hosted outright, or it is refused.
        QCOMPARE(Voices::engine(*chosen, false).error().code(), Failure::Code::HostedNotChosen);
        Voices::keep(*chosen, "voice", "coral");
        const CapabilityStatus status = AiRegistry::status(*chosen, Capability::TextToSpeech);
        QVERIFY(status.available);
        QVERIFY(status.hosted);
        QCOMPARE(status.provider, QString("OpenAI"));
        QCOMPARE(Voices::offered(*chosen), ServerVoice::openAiVoices());
        auto voice = AiRegistry::voice(*chosen);
        QVERIFY2(voice, qPrintable(Fixtures::whyNot(voice)));
        sayTo(**voice, server, Voices::speech(*chosen, "en"));
        const FakeServer::Request &asked = server.received.first();
        QVERIFY(asked.head.contains("Authorization: Bearer sk-voices"));
        QCOMPARE(asked.json().value("model").toString(), QString(ServerVoice::openAiModel));
        QCOMPARE(asked.json().value("voice").toString(), QString("coral"));
        chosen->setAiProvider("off");
        QCOMPARE(Voices::engine(*chosen).error().code(), Failure::Code::VoiceAiOff);
        QCOMPARE(AiRegistry::status(*chosen, Capability::TextToSpeech).why, CapabilityLack::AiOff);
        QVERIFY(AiEndpoints::keepKey(server.url(), {}, m_secrets));
    }
    // Gemini's voices by the same rule; they take no pace.
    void geminisVoicesReadOnlyWhereAllowed() {
        FakeServer server;
        auto chosen = settings({{"engine", "gemini"}});
        chosen->setGemini({{"url", server.url()}});
        QCOMPARE(Voices::engine(*chosen).error(), Failure(Failure::Code::VoiceServerHosted, {"Gemini"}));
        chosen = settings({{"engine", "gemini"}}, true);
        chosen->setGemini({{"url", server.url()}});
        QCOMPARE(AiRegistry::status(*chosen, Capability::TextToSpeech).why, CapabilityLack::NoKey);
        QVERIFY(Gemini::keepKey("gemini-secret", m_secrets));
        QCOMPARE(Voices::engine(*chosen).value_or("none"), QString("gemini"));
        QVERIFY(!Voices::paced(*chosen));
        Voices::keep(*chosen, "voice", "Puck");
        QCOMPARE(Voices::speech(*chosen, "en").voice, QString("Puck"));
        chosen->setAiProvider("off");
        QCOMPARE(Voices::engine(*chosen).error().code(), Failure::Code::VoiceAiOff);
        QVERIFY(Gemini::keepKey({}, m_secrets));
    }
    // With the AI off, a voice on this computer still reads: it only reads the text.
    void aLocalVoiceReadsWithTheAiOff() {
        const auto chosen = settings({{"engine", "speech-dispatcher"}});
        chosen->setAiProvider("off");
        const CapabilityStatus status = AiRegistry::status(*chosen, Capability::TextToSpeech);
        QVERIFY(status.available);
        QCOMPARE(status.provider, QString("speech-dispatcher"));
        QVERIFY(Voices::reader(*chosen).startsWith("speech-dispatcher"));
    }

    // Each provider keeps its own voice and pace: one's voice means nothing to another.
    void eachProviderKeepsItsVoiceAndPace() {
        const auto chosen = settings({{"engine", "piper"}, {"piper_program", bin("piper")}});
        Voices::keep(*chosen, "voice", "en_US-fake-medium");
        Voices::keep(*chosen, "rate", 1.2);
        QJsonObject setUp = chosen->readAloud();
        setUp.insert("engine", "speech-dispatcher");
        chosen->setReadAloud(setUp);
        QCOMPARE(Voices::speech(*chosen, {}), (Speech{.rate = 1.2, .voice = {}, .language = {}}));
        Voices::keep(*chosen, "voice", "male1");
        Voices::keep(*chosen, "rate", 0.8);
        setUp = chosen->readAloud();
        setUp.insert("engine", "piper");
        chosen->setReadAloud(setUp);
        QCOMPARE(Voices::speech(*chosen, "en"), (Speech{.rate = 1.2, .voice = "en_US-fake-medium", .language = "en"}));
        // An older Omabook reads the one in use's.
        QCOMPARE(chosen->readAloud().value("voice").toString(), QString("male1"));
        // And an older config.json's one voice stands for every engine.
        const auto older = settings({{"engine", "speech-dispatcher"}, {"voice", "female1"}, {"rate", 1.5}});
        QCOMPARE(Voices::speech(*older, {}), (Speech{.rate = 1.5, .voice = "female1", .language = {}}));
        // What reads changes with the engine and its set-up, not with the voice.
        const QString reading = Voices::reader(*older);
        Voices::keep(*older, "voice", "male2");
        QCOMPARE(Voices::reader(*older), reading);
        setUp = older->readAloud();
        setUp.insert("engine", "piper");
        setUp.insert("piper_program", bin("piper"));
        older->setReadAloud(setUp);
        QVERIFY(Voices::reader(*older) != reading);
    }

    // The AI page's preview reads a sentence with the voice chosen, and says why it can't.
    void thePreviewReadsASentenceWithTheVoiceChosen() {
        const auto chosen = settings({{"engine", "speech-dispatcher"}});
        Voices::keep(*chosen, "voice", "child_female");
        VoicePreview preview(chosen.get());
        QSignalSpy changed(&preview, &VoicePreview::changed);
        preview.play("This is how your books will be read aloud.", "nl");
        QVERIFY(preview.playing() || !changed.isEmpty());
        QTRY_VERIFY(!preview.playing());
        QCOMPARE(contents(log()).trimmed(), QString("spd-say -w -r 0 -t child_female -l nl -- This is how your books will be read aloud."));
        QVERIFY(preview.why().isEmpty());
        // Hosted, and not allowed: nothing is sent, and it says so.
        FakeServer server;
        const auto hosted = settings({{"engine", "openai"}, {"openai_url", server.url()}});
        VoicePreview refused(hosted.get());
        refused.play("Hi.", "en");
        QVERIFY(!refused.playing());
        QVERIFY(refused.why().contains("OpenAI"));
        QTest::qWait(50);
        QVERIFY(server.received.isEmpty());
    }
};

QTEST_MAIN(VoicesTests)
#include "voices_tests.moc"
