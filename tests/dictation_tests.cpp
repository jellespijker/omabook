// Dictation (#148): what is said for a note field, heard as a 16 kHz WAV and
// written out by the speech-to-text capability (ADR 0008), voxtype first, a
// hosted provider only where allowed and asked for, nothing with the AI Off;
// or voxtype's own dictation, started and stopped from Omabook, which types
// the words itself. The engines here are stand-ins: a recogniser that hears
// every recording as the same words, and a voxtype that answers as voxtype does.

#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <optional>

#include "audiodevices.h"
#include "dictation.h"
#include "dictationclip.h"
#include "failure.h"
#include "fixtures.h"
#include "recognition.h"
#include "settings.h"
#include "testaudio.h"
#include "voxtypedictation.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// A recogniser on this computer that hears every recording as the same
// words, and keeps the first bytes of the last file it was given.
class Hears : public Recognition::Recognizer {
public:
    Hears(QString words, QByteArray *head) : m_words(std::move(words)), m_head(head) {}
    [[nodiscard]] QString where() const override { return u"this computer"_s; }
    [[nodiscard]] QString model() const override { return u"hears-all"_s; }
    void recognize(const Recognition::Input &input) override {
        QFile file(input.file);
        if (file.open(QIODevice::ReadOnly))
            *m_head = file.read(44);
        QTimer::singleShot(20, this, [this] {
            emit recognized({.kind = Recognition::Kind::Audio,
                             .text = m_words,
                             .segments = {},
                             .language = {},
                             .model = model(),
                             .server = where(),
                             .at = 0,
                             .kept = false,
                             .timed = false});
        });
    }
    void cancel() override {}

private:
    QString m_words;
    QByteArray *m_head;
};

// voxtype, standing in: it writes down how it was called, transcribes any
// WAV as one sentence, and, with `no-daemon` beside it, says its daemon
// isn't running when asked to record.
[[nodiscard]] QString standInVoxtype(const QString &folder) {
    return Fixtures::script(folder + "/voxtype", R"sh(
here="$(dirname "$0")"
echo "$@" >> "$here/voxtype-calls"
case "$*" in
  *transcribe*)
    for last; do :; done
    head -c 4 "$last" > "$here/heard"
    echo "Loading audio file: $last"
    echo
    echo "Hello from the harbour."
    ;;
  "record start"*)
    if [ -f "$here/no-daemon" ]; then echo "Error: the voxtype daemon is not running" >&2; exit 1; fi
    ;;
esac
)sh");
}

[[nodiscard]] QStringList calls(const QString &folder) {
    QFile file(folder + "/voxtype-calls");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts) : QStringList();
}

// A computer with a microphone that hears a tone, voxtype standing in, and
// speech written out the ways `route` gives (the registry's when none).
struct Room {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    AudioDevices devices{*TestAudio::microphone("tone"), [] { return TestAudio::speakers(); }};
    QString voxtype = standInVoxtype(dir.path());
    QByteArray head;
    int routed = 0;
    std::optional<bool> hostedAsked;
    std::unique_ptr<Dictation> dictation;

    explicit Room(bool registry = false) {
        // whisper.cpp left as it is found: naming it would put it, and the server, before voxtype (#146).
        settings.setRecognition({{"voxtype_program", voxtype}});
        if (registry) {
            dictation = std::make_unique<Dictation>(&settings, &devices);
            return;
        }
        dictation = std::make_unique<Dictation>(&settings, &devices, [this](Recognition::Kind, bool hosted) {
            ++routed;
            hostedAsked = hosted;
            RecognitionRoute::Ways ways;
            ways.push_back(std::make_unique<Hears>(u"The lantern went out at nine."_s, &head));
            return std::expected<RecognitionRoute::Ways, QString>(std::move(ways));
        });
    }
    void choose(const QJsonObject &more) {
        QJsonObject chosen = settings.recognition();
        for (auto it = more.begin(); it != more.end(); ++it)
            chosen.insert(it.key(), it.value());
        settings.setRecognition(chosen);
    }
};

// What a field was given: its words, or why there were none.
struct Field {
    std::optional<QString> heard, failed;
    bool handedOver = false;
    Field(Dictation &dictation, const int &number) {
        QObject::connect(&dictation, &Dictation::heard, &dictation, [this, &number](int field, const QString &text) {
            if (field == number)
                heard = text;
        });
        QObject::connect(&dictation, &Dictation::failed, &dictation, [this, &number](int field, const QString &why) {
            if (field == number)
                failed = why;
        });
        QObject::connect(&dictation, &Dictation::handedOver, &dictation, [this, &number](int field) {
            if (field == number)
                handedOver = true;
        });
    }
};

// Waits until the microphone was heard for `ms` (more than the shortest
// written out), however late a busy computer hears it.
[[nodiscard]] bool heardFor(const Dictation &dictation, double ms) {
    return QTest::qWaitFor([&] { return dictation.elapsed() >= ms; }, 10000);
}

} // namespace

class DictationTests : public QObject {
    Q_OBJECT

private slots:
    // What is heard is kept at 16 kHz, a third of what the microphone gives,
    // and written as the WAV voxtype and whisper.cpp read as it is.
    void whatIsSaidIsKeptAsASixteenKilohertzWav() {
        AudioDevices devices(*TestAudio::microphone("tone"), [] { return TestAudio::speakers(); });
        auto clip = DictationClip::start(devices.microphone());
        QVERIFY2(clip, qPrintable(Fixtures::whyNot(clip)));
        // Waited for as heard, not as the clock goes: a busy computer hears late.
        QTRY_VERIFY_WITH_TIMEOUT((*clip)->elapsed() >= 500ms, 10000);
        QVERIFY((*clip)->level() > 0.3);
        (*clip)->stop();
        QVERIFY(!(*clip)->listening());
        QCOMPARE((*clip)->level(), 0.0);
        const auto heard = (*clip)->elapsed();
        QVERIFY2(heard >= 400ms && heard <= 1200ms, qPrintable(QString::number(heard.count())));
        QCOMPARE(static_cast<qint64>((*clip)->samples().size()), heard.count() * 16);
        // The tone, half as loud as can be, is still about that loud.
        const auto loudest = std::ranges::max((*clip)->samples(), {}, [](qint16 sample) { return std::abs(static_cast<int>(sample)); });
        QVERIFY(std::abs(static_cast<int>(loudest)) > 12000);

        QTemporaryDir dir;
        const QString file = dir.filePath("said.wav");
        QVERIFY((*clip)->write(file));
        QFile wav(file);
        QVERIFY(wav.open(QIODevice::ReadOnly));
        const QByteArray bytes = wav.readAll();
        QCOMPARE(bytes.left(4), QByteArray("RIFF"));
        QCOMPARE(bytes.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 22), quint16(1));     // mono
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 24), quint32(16000)); // samples a second
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 34), quint16(16));    // bits a sample
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 40), quint32((*clip)->samples().size() * 2));
        QCOMPARE(bytes.size(), 44 + (*clip)->samples().size() * 2);
    }

    // Listened for, stopped, written out: the field is given the words, and
    // the window could show all along that it listened, and for whom.
    void aFieldIsGivenWhatWasSaid() {
        Room room;
        Dictation &dictation = *room.dictation;
        QVERIFY(!dictation.listening());
        QSignalSpy changed(&dictation, &Dictation::changed);
        int number = 0;
        Field field(dictation, number);
        number = dictation.listen(u"Notebooks"_s, false);
        QVERIFY(number > 0);
        QVERIFY(dictation.listening());
        QCOMPARE(dictation.field(), number);
        QCOMPARE(dictation.who(), u"Notebooks"_s);
        QVERIFY(!dictation.byVoxtype());
        QVERIFY(heardFor(dictation, 400));
        QVERIFY(dictation.level() > 0.3);
        dictation.stop();
        QVERIFY(!dictation.listening());
        QVERIFY(dictation.writing());
        QTRY_VERIFY(field.heard.has_value());
        QCOMPARE(*field.heard, u"The lantern went out at nine."_s);
        QVERIFY(!field.failed);
        QVERIFY(!dictation.writing());
        QCOMPARE(dictation.field(), 0);
        QVERIFY(changed.count() >= 3);
        // It went as a WAV, and a plugin's view that didn't ask for hosted keeps to one's own.
        QCOMPARE(room.head.left(4), QByteArray("RIFF"));
        QCOMPARE(room.hostedAsked, std::optional<bool>(false));
        // Omabook's own fields ask for hosted where the settings allow it.
        int second = 0;
        Field again(dictation, second);
        second = dictation.listen({}, true);
        QVERIFY(heardFor(dictation, 400));
        dictation.stop();
        QTRY_VERIFY(again.heard.has_value() || again.failed.has_value());
        QVERIFY2(again.heard.has_value(), qPrintable(again.failed.value_or({})));
        QCOMPARE(room.hostedAsked, std::optional<bool>(true));
    }

    // With the AI Off nothing listens, nothing is written out, and voxtype's
    // own dictation isn't started either.
    void nothingListensWithTheAiOff() {
        Room room;
        room.settings.setAiProvider("off");
        room.dictation->setWithVoxtype(true);
        const QVariantMap status = room.dictation->status(true);
        QCOMPARE(status.value("available").toBool(), false);
        QCOMPARE(status.value("why").toString(), u"ai-off"_s);
        QCOMPARE(status.value("voxtype").toBool(), false);
        int number = 0;
        Field field(*room.dictation, number);
        number = room.dictation->listen({}, true);
        QVERIFY(!room.dictation->listening());
        QTRY_VERIFY(field.failed.has_value());
        QCOMPARE(*field.failed, Failure(Failure::Code::AiOff).text());
        QCOMPARE(room.routed, 0);
        QVERIFY(calls(room.dir.path()).isEmpty());
    }

    // Where speech to text has only a hosted server, a field that didn't ask
    // for hosted can't be dictated into, nor one that did while hosted isn't
    // allowed for speech to text; once it is, Omabook's own fields can.
    void aHostedProviderOnlyWhereAllowedAndAsked() {
        Room room(true);
        room.settings.setAiProvider("openai");
        room.choose({{"voxtype_program", room.dir.filePath("none/voxtype")}, {"transcription_url", "https://transcribe.example.com/v1"}});
        QVERIFY(!room.dictation->status(false).value("available").toBool());
        QCOMPARE(room.dictation->status(true).value("why").toString(), u"hosted-not-allowed"_s);
        int number = 0;
        Field field(*room.dictation, number);
        number = room.dictation->listen({}, true);
        QVERIFY(!room.dictation->listening());
        QTRY_VERIFY(field.failed.has_value());
        QCOMPARE(*field.failed, room.dictation->status(true).value("message").toString());

        room.settings.setCapabilities({{"speech-to-text", QJsonObject{{"hosted", true}}}});
        const QVariantMap mine = room.dictation->status(true);
        QVERIFY2(mine.value("available").toBool(), qPrintable(mine.value("message").toString()));
        QVERIFY(mine.value("hosted").toBool());
        QCOMPARE(room.dictation->status(false).value("why").toString(), u"hosted-not-chosen"_s);
    }

    // Through the registry, as the window has it: voxtype on this computer
    // writes out what was said, from the WAV as it is.
    void voxtypeWritesOutWhatWasSaid() {
        Room room(true);
        room.settings.setAiProvider("openai");
        const QVariantMap status = room.dictation->status(false);
        QVERIFY2(status.value("available").toBool(), qPrintable(status.value("message").toString()));
        QCOMPARE(status.value("provider").toString(), u"voxtype on this computer"_s);
        QVERIFY(!status.value("hosted").toBool());
        int number = 0;
        Field field(*room.dictation, number);
        number = room.dictation->listen({}, true);
        QVERIFY(heardFor(*room.dictation, 400));
        room.dictation->stop();
        QTRY_VERIFY_WITH_TIMEOUT(field.heard.has_value() || field.failed.has_value(), 10000);
        QVERIFY2(field.heard.has_value(), qPrintable(field.failed.value_or({})));
        QCOMPARE(*field.heard, u"Hello from the harbour."_s);
        QFile heard(room.dir.filePath("heard"));
        QVERIFY(heard.open(QIODevice::ReadOnly));
        QCOMPARE(heard.readAll(), QByteArray("RIFF"));
        // The recording is gone once it was written out.
        const QStringList called = calls(room.dir.path());
        QCOMPARE(called.size(), 1);
        QVERIFY(!QFile::exists(called.first().section(' ', -1)));
    }

    // A press of the key is no dictation; a second field waits for the
    // first; and a field left while it is written out is given nothing.
    void aFieldIsListenedForOnlyWhileItAsks() {
        Room room;
        Dictation &dictation = *room.dictation;
        int first = 0;
        Field one(dictation, first);
        first = dictation.listen({}, true);
        QTest::qWait(50);
        dictation.stop();
        QTRY_VERIFY(one.failed.has_value());
        QCOMPARE(*one.failed, Failure(Failure::Code::DictationNothingHeard).text());
        QCOMPARE(room.routed, 0);

        int second = 0;
        int third = 0;
        Field two(dictation, second);
        Field three(dictation, third);
        second = dictation.listen({}, true);
        third = dictation.listen(u"Notebooks"_s, false);
        QTRY_VERIFY(three.failed.has_value());
        QCOMPARE(*three.failed, Failure(Failure::Code::DictationBusy).text());
        QCOMPARE(dictation.field(), second);
        QVERIFY(heardFor(dictation, 400));
        dictation.stop();
        QVERIFY(dictation.writing());
        dictation.forget(third); // not the one written for: nothing changes
        QVERIFY(dictation.writing());
        dictation.forget(second);
        QVERIFY(!dictation.writing());
        QVERIFY(!dictation.listening());
        QTest::qWait(200);
        QVERIFY(!two.heard && !two.failed);

        // Cancelled while it listens: nothing is written out.
        int fourth = 0;
        Field four(dictation, fourth);
        const int routedBefore = room.routed;
        fourth = dictation.listen({}, true);
        QVERIFY(heardFor(dictation, 400));
        dictation.cancel();
        QVERIFY(!dictation.listening());
        QTest::qWait(200);
        QCOMPARE(room.routed, routedBefore);
        QVERIFY(!four.heard && !four.failed);
    }

    // voxtype's own dictation, where chosen: started and stopped from
    // Omabook, typing the words itself, with nothing pressed after them.
    void voxtypesOwnDictationIsStartedAndStopped() {
        Room room;
        Dictation &dictation = *room.dictation;
        QVERIFY(dictation.voxtypeFound());
        QVERIFY(!dictation.withVoxtype());
        dictation.setWithVoxtype(true);
        QVERIFY(dictation.withVoxtype());
        QCOMPARE(room.settings.recognition().value("dictation").toString(), u"voxtype"_s);
        QCOMPARE(dictation.status(false).value("voxtype").toBool(), true);
        QCOMPARE(dictation.status(false).value("hosted").toBool(), false);
        int number = 0;
        Field field(dictation, number);
        number = dictation.listen({}, true);
        QVERIFY(dictation.listening());
        QVERIFY(dictation.byVoxtype());
        QTRY_COMPARE(calls(room.dir.path()), QStringList{u"record start --type --no-auto-submit --no-osd"_s});
        dictation.stop();
        QVERIFY(!dictation.listening());
        QVERIFY(field.handedOver);
        QTRY_COMPARE(calls(room.dir.path()).size(), 2);
        QCOMPARE(calls(room.dir.path()).last(), u"record stop"_s);
        QCOMPARE(room.routed, 0);

        // Cancelled, voxtype drops it.
        int second = 0;
        Field two(dictation, second);
        second = dictation.listen({}, true);
        dictation.cancel();
        QTRY_COMPARE(calls(room.dir.path()).size(), 4);
        QCOMPARE(calls(room.dir.path()).last(), u"record cancel"_s);
        QVERIFY(!two.handedOver);

        // Its daemon not running: the field is told so, in voxtype's words.
        QVERIFY(Fixtures::write(room.dir.filePath("no-daemon"), "1"));
        int third = 0;
        Field three(dictation, third);
        third = dictation.listen({}, true);
        QTRY_VERIFY(three.failed.has_value());
        QVERIFY2(three.failed->contains("daemon is not running"), qPrintable(*three.failed));
        QVERIFY(!dictation.listening());

        dictation.setWithVoxtype(false);
        QVERIFY(!room.settings.recognition().contains("dictation"));
        QCOMPARE(VoxtypeDictation::arguments(u"stop"_s), (QStringList{u"record"_s, u"stop"_s}));
    }
};

QTEST_MAIN(DictationTests)
#include "dictation_tests.moc"
