// Writing out recordings (#151): a recording of the library, or a plugin's,
// written out through the speech-to-text capability a piece at a time, by
// voxtype, whisper.cpp or a transcription server (stand-ins here: scripts
// and a server on this computer), each word with its moment from the start
// of the recording where the way gives them; kept as each piece is written
// out, so a long recording stops and goes on from where it was; nothing
// read with the AI Off and a hosted way only when allowed and asked for; a
// recording in the library read as the book of its transcript and found by
// the shelf's search; and a plugin's, in one call.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include "bookwords.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "formats.h"
#include "library.h"
#include "opusreader.h"
#include "opuswriter.h"
#include "recordingbook.h"
#include "recordingpieces.h"
#include "scriptplugin.h"
#include "secretstore.h"
#include "settings.h"
#include "transcriber.h"
#include "transcriptstore.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

using Fixtures::whyNot;
using Outcome = std::expected<Recognition::Result, QString>;

// A tone, `seconds` long, as a WAV of 16-bit samples.
[[nodiscard]] bool wav(const QString &path, double seconds, int rate = 16000, int channels = 1) {
    const auto frames = static_cast<qint64>(seconds * rate);
    QByteArray data;
    for (qint64 frame = 0; frame < frames; ++frame)
        for (int channel = 0; channel < channels; ++channel) {
            std::array<char, 2> two{};
            qToLittleEndian(static_cast<qint16>(8000 * std::sin(static_cast<double>(frame) / 9.0)), two.data());
            data.append(two.data(), 2);
        }
    QByteArray file("RIFF");
    file += Fixtures::be32(0); // a recorder that never wrote the size: it isn't needed
    file += "WAVEfmt ";
    const auto le32 = [](quint32 value) {
        std::array<char, 4> four{};
        qToLittleEndian(value, four.data());
        return QByteArray(four.data(), 4);
    };
    const auto le16 = [](quint16 value) {
        std::array<char, 2> two{};
        qToLittleEndian(value, two.data());
        return QByteArray(two.data(), 2);
    };
    file += le32(16) + le16(1) + le16(static_cast<quint16>(channels)) + le32(static_cast<quint32>(rate))
            + le32(static_cast<quint32>(rate * channels * 2)) + le16(static_cast<quint16>(channels * 2)) + le16(16);
    file += "data" + le32(static_cast<quint32>(data.size())) + data;
    return Fixtures::write(path, file);
}

// voxtype's command, standing in: what it says as `voxtype transcribe` does,
// the words of the how-manieth recording it was given, and a line in `calls`
// for each.
[[nodiscard]] QString standInVoxtype(const QString &folder) {
    return Fixtures::script(folder + "/voxtype", R"sh(
for last; do :; done
echo "$last" >> "$(dirname "$0")/calls"
n=$(wc -l < "$(dirname "$0")/calls")
echo "Loading audio file: $last"
echo
echo "Words of piece $n."
)sh");
}

// whisper.cpp's command, standing in: two words, the second at 0.4 s, in each piece.
[[nodiscard]] QString standInWhisper(const QString &folder) {
    return Fixtures::script(folder + "/whisper-cli", R"sh(
echo "$@" >> "$(dirname "$0")/calls"
while [ $# -gt 0 ]; do [ "$1" = "-of" ] && out="$2"; shift; done
cat > "$out.json" <<'JSON'
{"result": {"language": "en"}, "transcription": [
 {"offsets": {"from": 0, "to": 400}, "text": " Good"},
 {"offsets": {"from": 400, "to": 900}, "text": " morning."}]}
JSON
)sh");
}

[[nodiscard]] int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? static_cast<int>(file.readAll().count('\n')) : 0;
}

// A computer with its settings and the transcriber, writing out in pieces of
// a second; the AI server is a stand-in here (this computer: one's own).
struct Desk {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    std::unique_ptr<Transcriber> transcriber;
    QString bin = dir.filePath("bin");

    Desk() {
        QDir().mkpath(bin);
        settings.setAiProvider("openai");
        settings.setAiEndpoint({{"preset", "custom"}, {"base_url", ""}});
        choose({{"voxtype_program", standInVoxtype(bin)}});
        transcriber = std::make_unique<Transcriber>(RecognitionQueue::routeFor(settings), dir.filePath("transcripts"));
        transcriber->setPieceLength(1s);
    }
    void choose(const QJsonObject &recognition) { settings.setRecognition(recognition); }
    [[nodiscard]] int calls() const { return lines(bin + "/calls"); }

    // Writes it out, and waits for what comes of it; the progress told, as "done/total".
    Outcome transcribe(Transcriber::Request request, QStringList *told = nullptr,
                       const std::function<void(Transcriber::Progress)> &also = {}) {
        std::optional<Outcome> outcome;
        transcriber->transcribe(
            std::move(request), [&outcome](const Outcome &done) { outcome = done; },
            [told, also](Transcriber::Progress at) {
                if (told)
                    *told << u"%1/%2"_s.arg(at.done).arg(at.total);
                if (also)
                    also(at);
            });
        [&] { QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 20000); }();
        return outcome.value_or(std::unexpected(u"nothing came"_s));
    }
};

[[nodiscard]] Transcriber::Request of(const QString &file, bool hosted = false) {
    return {.file = file, .language = {}, .hosted = hosted, .again = false, .key = {}};
}

[[nodiscard]] QStringList starts(const Recognition::Result &result) {
    QStringList found;
    for (const Recognition::Segment &segment : result.segments)
        found << QString::number(segment.start);
    return found;
}

} // namespace

class TranscriptionTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;
    QTemporaryDir m_data;

private slots:
    void initTestCase() {
        Secrets::useInstead(&m_secrets);
        // Where transcripts are kept, and whisper.cpp's models looked for: here.
        qputenv("XDG_DATA_HOME", m_data.filePath("home").toUtf8());
    }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    // A WAV of any rate and channels, and an Opus recording (a plugin's), are
    // cut by Omabook itself into pieces of 16 kHz mono.
    void recordingsAreCutIntoPiecesOfSixteenKilohertz() {
        Desk desk;
        const QString stereo = desk.dir.filePath("stereo.wav");
        QVERIFY(wav(stereo, 2.5, 44100, 2));
        const auto length = RecordingPieces::length(stereo);
        QVERIFY2(length, qPrintable(whyNot(length)));
        QVERIFY(*length == 2500ms);
        const QString piece = desk.dir.filePath("piece.wav");
        const auto cut = RecordingPieces::cut(stereo, 2000ms, 1000ms, piece);
        QVERIFY2(cut, qPrintable(whyNot(cut)));
        // Half a second is left at the end: 8000 samples of 16 kHz, after a header of 44 bytes.
        QCOMPARE(QFileInfo(piece).size(), qint64{44 + 8000 * 2});
        QVERIFY(RecordingPieces::length(piece).value_or(0ms) == 500ms);

        const QString opus = desk.dir.filePath("talk.ogg");
        {
            auto writer = Opus::Writer::open({.path = opus, .started = QDateTime::currentDateTime(), .bitrate = 32000});
            QVERIFY2(writer, qPrintable(whyNot(writer)));
            std::vector<qint16> samples(static_cast<std::size_t>(Opus::sampleRate) * 3, 1000);
            QVERIFY((*writer)->write(samples));
            QVERIFY((*writer)->finish());
        }
        QVERIFY(RecordingPieces::length(opus).value_or(0ms) == 3000ms);
        QVERIFY(RecordingPieces::cut(opus, 1000ms, 1000ms, piece));
        QCOMPARE(QFileInfo(piece).size(), qint64{44 + 16000 * 2});
    }

    // voxtype, first where nothing else was chosen: a piece at a time, in
    // order, the words without their moments, and each piece's from the
    // recording's start. Kept whole, it is answered without asking again.
    void voxtypeWritesARecordingOutAPieceAtATime() {
        Desk desk;
        const QString talk = desk.dir.filePath("talk.wav");
        QVERIFY(wav(talk, 2.5));
        QStringList told;
        const Outcome heard = desk.transcribe(of(talk), &told);
        QVERIFY2(heard, qPrintable(whyNot(heard)));
        QCOMPARE(heard->text, u"Words of piece 1. Words of piece 2. Words of piece 3."_s);
        QCOMPARE(heard->server, u"voxtype on this computer"_s);
        QVERIFY(!heard->timed);
        QCOMPARE(starts(*heard), (QStringList{"0", "1000", "2000"}));
        QCOMPARE(told, (QStringList{"0/3", "1/3", "2/3", "3/3"}));
        QCOMPARE(desk.calls(), 3);

        const Outcome again = desk.transcribe(of(talk));
        QVERIFY2(again && again->kept, qPrintable(whyNot(again)));
        QCOMPARE(again->text, heard->text);
        QCOMPARE(desk.calls(), 3);
        const auto key = TranscriptStore::keyOf(desk.transcriber->folder(), talk);
        QVERIFY(key);
        const Transcriber::Kept kept = desk.transcriber->kept(*key);
        QVERIFY(kept.whole);
        QCOMPARE(kept.total, 3);
        // Asked anew, it is.
        Transcriber::Request anew = of(talk);
        anew.again = true;
        const Outcome redone = desk.transcribe(anew);
        QVERIFY2(redone && !redone->kept, qPrintable(whyNot(redone)));
        QCOMPARE(desk.calls(), 6);
    }

    // whisper.cpp gives each word its moment: counted from the recording's
    // start, not from its piece's.
    void whisperGivesEachWordItsMomentInTheRecording() {
        Desk desk;
        desk.choose({{"voxtype_program", desk.dir.filePath("none/voxtype")},
                     {"whisper_program", standInWhisper(desk.bin)},
                     {"whisper_model", desk.dir.filePath("ggml-base.bin")}});
        QVERIFY(Fixtures::write(desk.dir.filePath("ggml-base.bin"), "model"));
        const QString talk = desk.dir.filePath("talk.wav");
        QVERIFY(wav(talk, 1.5));
        const Outcome heard = desk.transcribe(of(talk));
        QVERIFY2(heard, qPrintable(whyNot(heard)));
        QVERIFY(heard->timed);
        QCOMPARE(heard->server, u"whisper.cpp on this computer"_s);
        QList<qint64> moments;
        for (const Recognition::Segment &segment : heard->segments)
            for (const Recognition::Word &word : segment.words)
                moments << word.start;
        QCOMPARE(moments, (QList<qint64>{0, 400, 1000, 1400}));
        QCOMPARE(heard->text, u"Good morning. Good morning."_s);
    }

    // A transcription server of one's own writes it out too, each word timed.
    void aServerOfOnesOwnWritesItOut() {
        Desk desk;
        FakeServer server;
        server.answering = [](const FakeServer::Request &) {
            const QJsonObject answer{{"text", " Hello there."},
                                     {"segments", QJsonArray{QJsonObject{{"start", 0.0}, {"end", 0.6}, {"text", " Hello there."}}}},
                                     {"words", QJsonArray{QJsonObject{{"word", "Hello"}, {"start", 0.0}, {"end", 0.3}},
                                                          QJsonObject{{"word", "there."}, {"start", 0.3}, {"end", 0.6}}}}};
            return FakeServer::Reply{.status = "200 OK", .body = QJsonDocument(answer).toJson()};
        };
        desk.choose({{"voxtype_program", desk.dir.filePath("none/voxtype")}, {"transcription_url", server.url()}});
        const QString talk = desk.dir.filePath("talk.wav");
        QVERIFY(wav(talk, 1.2));
        const Outcome heard = desk.transcribe(of(talk));
        QVERIFY2(heard, qPrintable(whyNot(heard)));
        QVERIFY(heard->timed);
        QCOMPARE(server.received.size(), 2);
        QCOMPARE(heard->segments.size(), 2);
        QCOMPARE(heard->segments.at(1).words.at(1).start, qint64{1300});
    }

    // A long recording is stopped part way: what was written out is kept, and
    // started again it goes on from the piece it was at, asking for no piece twice.
    void aLongRecordingStopsAndGoesOnWhereItWas() {
        Desk desk;
        const QString talk = desk.dir.filePath("lecture.wav");
        QVERIFY(wav(talk, 4.0));
        std::optional<Outcome> outcome;
        int job = 0;
        job = desk.transcriber->transcribe(
            of(talk), [&outcome](const Outcome &done) { outcome = done; },
            [&desk, &job](Transcriber::Progress at) {
                if (at.done == 2)
                    QVERIFY(desk.transcriber->stop(job));
            });
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 20000);
        QVERIFY(!*outcome);
        QVERIFY2(outcome->error().contains("stopped"), qPrintable(whyNot(*outcome)));
        QCOMPARE(desk.calls(), 2);
        const auto key = TranscriptStore::keyOf(desk.transcriber->folder(), talk);
        QVERIFY(key);
        const Transcriber::Kept part = desk.transcriber->kept(*key);
        QCOMPARE(part.done, 2);
        QCOMPARE(part.total, 4);
        QVERIFY(!part.whole);
        // Nothing of it reads as a book yet.
        QVERIFY(!desk.transcriber->stop(job));

        QStringList told;
        const Outcome heard = desk.transcribe(of(talk), &told);
        QVERIFY2(heard, qPrintable(whyNot(heard)));
        QCOMPARE(told.first(), u"2/4"_s);
        QCOMPARE(desk.calls(), 4);
        QCOMPARE(starts(*heard), (QStringList{"0", "1000", "2000", "3000"}));
        QCOMPARE(heard->text, u"Words of piece 1. Words of piece 2. Words of piece 3. Words of piece 4."_s);
        // One waiting its turn is stopped too, and is told so.
        std::optional<Outcome> first, second;
        const QString other = desk.dir.filePath("other.wav");
        QVERIFY(wav(other, 1.0));
        desk.transcriber->transcribe(of(other), [&first](const Outcome &done) { first = done; });
        const int waiting = desk.transcriber->transcribe(of(talk, true), [&second](const Outcome &done) { second = done; });
        QVERIFY(desk.transcriber->stop(waiting));
        QVERIFY(second.has_value() && !*second);
        QTRY_VERIFY_WITH_TIMEOUT(first.has_value(), 20000);
        QVERIFY2(*first, qPrintable(whyNot(*first)));
    }

    // With the AI Off nothing is read of the recording, nor asked of any way.
    void nothingIsWrittenOutWithTheAiOff() {
        Desk desk;
        desk.settings.setAiProvider("off");
        const QString talk = desk.dir.filePath("talk.wav");
        QVERIFY(wav(talk, 1.0));
        const Outcome refused = desk.transcribe(of(talk, true));
        QVERIFY(!refused);
        QCOMPARE(desk.calls(), 0);
        QVERIFY(!QDir(desk.transcriber->folder()).exists());
    }

    // A server that is someone else's computer is sent pieces only when hosted
    // is allowed for speech to text and the one asking asked for it.
    void aHostedWayOnlyWhenAllowedAndAsked() {
        Desk desk;
        FakeServer hosted;
        hosted.answering = [](const FakeServer::Request &) {
            return FakeServer::Reply{.status = "200 OK", .body = R"({"text": "From afar."})"};
        };
        desk.choose({{"voxtype_program", desk.dir.filePath("none/voxtype")},
                     {"transcription_url", hosted.url()},
                     {"transcription_model", "whisper:cloud"}});
        const QString talk = desk.dir.filePath("talk.wav");
        QVERIFY(wav(talk, 1.0));
        QVERIFY(!desk.transcribe(of(talk, true)));
        desk.settings.setRecognitionHosted(true);
        QVERIFY(!desk.transcribe(of(talk, false)));
        QCOMPARE(hosted.received.size(), 0);
        const Outcome sent = desk.transcribe(of(talk, true));
        QVERIFY2(sent, qPrintable(whyNot(sent)));
        QCOMPARE(sent->text, u"From afar."_s);
        QCOMPARE(hosted.received.size(), 1);
    }

    // A recording in the library is a book once it is written out: before,
    // opening it says how; after, it reads as its transcript, in chapters by
    // their moment, and the shelf's search finds it by what is said in it.
    void aRecordingInTheLibraryReadsAsWhatIsSaidInIt() {
        Desk desk;
        for (const QString &extension : RecordingBook::extensions())
            QVERIFY2(Formats::readable(extension) && Formats::isBook(u"a."_s + extension), qPrintable(extension));
        Library library(desk.dir.filePath("Library"));
        QVERIFY(library.open());
        const QString talk = desk.dir.filePath("Notes on the Engine - Ada Lovelace.wav");
        QVERIFY(wav(talk, 1.5));
        const Library::Added added = library.add(talk, Formats::readMeta(talk));
        QVERIFY2(added.id > 0, qPrintable(added.error));
        const auto book = library.book(added.id);
        QVERIFY(book);
        QCOMPARE(book->meta.title, u"Notes on the Engine"_s);
        const auto file = library.formatFiles(book->id).value(0);
        const auto before = Formats::opened(file.path);
        QVERIFY(!before);
        QCOMPARE(before.error().code(), Failure::Code::NotTranscribed);

        Transcriber transcriber(RecognitionQueue::routeFor(desk.settings));
        transcriber.setPieceLength(1s);
        std::optional<Outcome> outcome;
        transcriber.transcribe({.file = file.path, .language = {}, .hosted = true, .again = false, .key = file.hash},
                               [&outcome](const Outcome &done) { outcome = done; });
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 20000);
        QVERIFY2(*outcome, qPrintable(whyNot(*outcome)));
        const auto opened = Formats::opened(file.path);
        QVERIFY2(opened, qPrintable(whyNot(opened)));
        QCOMPARE((*opened)->count(), 1);
        QVERIFY2((*opened)->text(0).contains("Words of piece 2."), qPrintable((*opened)->text(0)));
        QVERIFY(BookWords::index(library, book->id, RecordingBook::extensions()));
        QCOMPARE(BookWords::books(library, {u"piece 2"_s}), (QSet<qint64>{book->id}));
    }

    // Chapters of ten minutes, each named by when it begins; a word's own
    // text never read as markup.
    void aTranscriptIsABookOfChaptersByTheirMoment() {
        QJsonArray segments;
        for (const qint64 at : {qint64{0}, qint64{65'000}, qint64{12 * 60'000}, qint64{75 * 60'000}})
            segments.append(QJsonObject{{"text", u"Said <at> %1."_s.arg(at)}, {"start", at}, {"end", at + 1000}});
        const BookDescription book = RecordingBook::describe({{"segments", segments}, {"language", "en"}}, BookMeta{});
        QCOMPARE(book.chapters.size(), 3);
        QCOMPARE(book.chapters.at(0).title, u"0:00"_s);
        QCOMPARE(book.chapters.at(1).title, u"10:00"_s);
        QCOMPARE(book.chapters.at(2).title, u"1:10:00"_s);
        QVERIFY(book.chapters.at(0).html.contains("&lt;at&gt; 65000."));
        QCOMPARE(book.meta.language, u"en"_s);
    }

    // A plugin writes out a recording of its own in one call, told how far it
    // is; stopped, a later call goes on where it was.
    void aPluginWritesOutARecordingInOneCall() {
        Desk desk;
        Library library(desk.dir.filePath("Library"));
        QVERIFY(library.open());
        const QString folder = desk.dir.filePath("plugins/notebook");
        QDir().mkpath(folder + "/data");
        Fixtures::write(folder + "/plugin.json", R"({"name": "Notebook", "version": "1.0", "script": "main.js"})");
        Fixtures::write(folder + "/main.js", R"js(
var told = [];
var asked = null;
function write(file) {
    told = [];
    asked = omabook.recognize.transcript(file, {onProgress: function (done, total) { told.push(done + "/" + total); }});
    return asked;
}
function startAndStop(file) {
    var stopped = null;
    var writing = omabook.recognize.transcript(file, {onProgress: function (done) { if (done === 1) stopped = writing.stop(); }});
    return writing.then(function () { return "finished"; }, function (error) { return String(stopped) + ": " + error; });
}
function progress() { return told; }
function wrong() { try { omabook.recognize.transcript(3); return ""; } catch (error) { return String(error.message); } }
)js");
        QVERIFY(wav(folder + "/data/talk.wav", 3.0));
        ScriptServices services;
        services.settings = &desk.settings;
        services.library = [&library] { return &library; };
        services.transcriber = desk.transcriber.get();
        ScriptPlugin plugin(ScriptManifest::read(folder), services);
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        const auto call = [&plugin](const QString &function, const QJsonArray &arguments) {
            std::optional<std::pair<QJsonValue, QString>> answer;
            plugin.callFunction(function, arguments, [&answer](const QJsonValue &value, const QString &why) { answer = {{value, why}}; });
            [&] { QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 20000); }();
            return answer.value_or(std::pair<QJsonValue, QString>{});
        };

        const auto [stopped, stopWhy] = call(u"startAndStop"_s, {u"data/talk.wav"_s});
        QVERIFY2(stopWhy.isEmpty(), qPrintable(stopWhy));
        QVERIFY2(stopped.toString().startsWith("true: ") && stopped.toString().contains("stopped"), qPrintable(stopped.toString()));
        QCOMPARE(desk.calls(), 1);

        const auto [heard, why] = call(u"write"_s, {u"data/talk.wav"_s});
        QVERIFY2(why.isEmpty(), qPrintable(why));
        const QJsonObject result = heard.toObject();
        QCOMPARE(result.value("text").toString(), u"Words of piece 1. Words of piece 2. Words of piece 3."_s);
        QCOMPARE(result.value("timed").toBool(true), false);
        QCOMPARE(result.value("segments").toArray().size(), 3);
        QCOMPARE(desk.calls(), 3);
        QCOMPARE(plugin.callNow(u"progress"_s, {}, &problem).toArray(), (QJsonArray{"1/3", "2/3", "3/3"}));
        QCOMPARE(plugin.callNow(u"wrong"_s, {}, &problem).toString(), u"The recording should be a string."_s);
        // Outside its own folder, a recording is refused.
        const auto [none, refused] = call(u"write"_s, {desk.dir.filePath("talk.wav")});
        QVERIFY2(refused.contains("plugin's own folder"), qPrintable(refused));
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }
};

QTEST_GUILESS_MAIN(TranscriptionTests)
#include "transcription_tests.moc"
