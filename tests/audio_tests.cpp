// Recording and playing sound for plugins (#79): Opus in Ogg written as it
// is heard and read back from anywhere in it, a recording paused and cut
// short, a player that seeks and says where it is, two hours that stay
// sound, and a stand-in plugin that records with the person's yes and plays
// it back. The build machine has no sound card: the microphone and the
// speakers are stand-ins that the tests feed and drain at their own pace.
// Nor has it the desktop's devices: a stand-in for them says when the
// default output or input changes, which what plays and listens follows (#173).

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>

#include "audiodevices.h"
#include "audioplayer.h"
#include "audiorecorder.h"
#include "fakesounddevices.h"
#include "fixtures.h"
#include "localvoices.h"
#include "opusreader.h"
#include "opuswriter.h"
#include "scriptapi.h"
#include "scriptplugin.h"
#include "settings.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

using Fixtures::whyNot;

// A tone of 440 Hz, as loud in each second as `loudness` says. A second of
// it is whole cycles, so one second is worked out once.
[[nodiscard]] QList<qint16> tone(int fromSecond, int seconds, const std::function<double(int second)> &loudness) {
    static const QList<double> wave = [] {
        QList<double> one(Opus::sampleRate);
        for (int i = 0; i < Opus::sampleRate; ++i)
            one[i] = std::sin(2 * std::numbers::pi * 440 * i / Opus::sampleRate);
        return one;
    }();
    QList<qint16> samples;
    samples.reserve(static_cast<qsizetype>(seconds) * Opus::sampleRate);
    for (int second = fromSecond; second < fromSecond + seconds; ++second) {
        const double peak = loudness(second);
        for (const double value : wave)
            samples << static_cast<qint16>(peak * value);
    }
    return samples;
}

[[nodiscard]] QByteArray bytesOf(const QList<qint16> &samples) {
    QByteArray bytes(samples.size() * 2, '\0');
    std::memcpy(bytes.data(), samples.constData(), static_cast<std::size_t>(bytes.size()));
    return bytes;
}

// How loud a stretch is: the root of its mean square.
[[nodiscard]] double loudness(std::span<const qint16> samples) {
    double sum = 0;
    for (const qint16 sample : samples)
        sum += static_cast<double>(sample) * sample;
    return samples.empty() ? 0 : std::sqrt(sum / static_cast<double>(samples.size()));
}

// Reads `seconds` of a reader from where it is, and says how loud they were.
[[nodiscard]] double loudnessRead(Opus::Reader &reader, double seconds) {
    QList<qint16> samples(static_cast<qsizetype>(seconds * Opus::sampleRate));
    samples.resize(reader.read(std::span<qint16>(samples.data(), static_cast<std::size_t>(samples.size()))));
    return loudness(std::span<const qint16>(samples.constData(), static_cast<std::size_t>(samples.size())));
}

// A sine of peak p has a root mean square of p / sqrt(2); Opus keeps it near.
[[nodiscard]] bool near(double heard, double peak) {
    const double expected = peak / std::numbers::sqrt2;
    return std::abs(heard - expected) < expected * 0.2;
}

// What the stand-in microphone hears, as its device gives it.
class Heard final : public QIODevice {
public:
    Heard() { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
    void push(const QByteArray &bytes) {
        m_bytes += bytes;
        emit readyRead();
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_bytes.size() + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 most) override {
        const qint64 taken = std::min<qint64>(most, m_bytes.size());
        std::memcpy(data, m_bytes.constData(), static_cast<std::size_t>(taken));
        m_bytes.remove(0, taken);
        return taken;
    }
    qint64 writeData(const char * /*data*/, qint64 /*size*/) override { return -1; }

private:
    QByteArray m_bytes;
};

class FakeMicrophone final : public AudioInput {
public:
    std::expected<QIODevice *, QString> start() override {
        started = true;
        return &m_heard;
    }
    void suspend() override { suspended = true; }
    void resume() override { suspended = false; }
    void stop() override { started = false; }
    // What it hears, as a microphone that is suspended doesn't.
    void hear(const QList<qint16> &samples) {
        if (started && !suspended)
            m_heard.push(bytesOf(samples));
    }
    void breakDown() { emit failed(u"The microphone was unplugged."_s); }

    bool started = false, suspended = false;

private:
    Heard m_heard;
};

class FakeSpeakers final : public AudioOutput {
public:
    std::expected<void, QString> start(QIODevice &source) override {
        m_source = &source;
        m_played = 0;
        suspended = false;
        ++starts;
        return {};
    }
    void suspend() override { suspended = true; }
    void resume() override { suspended = false; }
    void stop() override { m_source = nullptr; }
    std::chrono::microseconds played() const override { return std::chrono::microseconds(m_played * 1'000'000 / (Opus::sampleRate * 2)); }

    // Plays this long, as a sound card takes it; what it got, in samples.
    qint64 take(std::chrono::milliseconds time) {
        if (m_source == nullptr || suspended)
            return 0;
        const QByteArray got = m_source->read(time.count() * Opus::sampleRate / 1000 * 2);
        m_played += got.size();
        heard += got;
        if (m_source->atEnd())
            emit drained();
        return got.size() / 2;
    }

    bool suspended = false;
    int starts = 0;
    QByteArray heard;

private:
    QIODevice *m_source = nullptr;
    qint64 m_played = 0;
};

[[nodiscard]] QList<QByteArray> movedTo(const QSignalSpy &spy) {
    QList<QByteArray> ids;
    for (const QList<QVariant> &arguments : spy)
        ids << arguments.first().value<SoundDevice>().id;
    return ids;
}

// A second of silence as a WAV file, as Piper writes a sentence.
[[nodiscard]] QByteArray wavOfSilence() {
    const QByteArray samples(48000 * 2, '\0');
    const auto le = [](quint32 value, int size) {
        QByteArray bytes;
        for (int i = 0; i < size; ++i)
            bytes.append(static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xffU));
        return bytes;
    };
    return "RIFF" + le(36 + samples.size(), 4) + "WAVEfmt " + le(16, 4) + le(1, 2) + le(1, 2) + le(48000, 4) + le(96000, 4) + le(2, 2)
           + le(16, 2) + "data" + le(samples.size(), 4) + samples;
}

// Devices whose microphone and speakers are the stand-ins, each kept as it
// is made so the test can feed and drain it.
struct Room {
    QPointer<FakeMicrophone> microphone;
    QPointer<FakeSpeakers> speakers;
    AudioDevices devices{[this] {
                             auto made = std::make_unique<FakeMicrophone>();
                             microphone = made.get();
                             return std::unique_ptr<AudioInput>(std::move(made));
                         },
                         [this] {
                             auto made = std::make_unique<FakeSpeakers>();
                             speakers = made.get();
                             return std::unique_ptr<AudioOutput>(std::move(made));
                         }};
};

[[nodiscard]] std::unique_ptr<Opus::Reader> readerOf(const QString &path) {
    auto reader = Opus::Reader::open(path);
    return reader ? std::move(*reader) : nullptr;
}

// Writes a recording of `seconds`, each second as loud as 2000 times its number and one.
[[nodiscard]] bool record(const QString &path, int seconds) {
    auto writer = Opus::Writer::open({.path = path, .started = QDateTime::currentDateTimeUtc(), .bitrate = 32000});
    if (!writer)
        return false;
    const QList<qint16> samples = tone(0, seconds, [](int second) { return 2000.0 * (second + 1); });
    return (*writer)->write(std::span<const qint16>(samples.constData(), static_cast<std::size_t>(samples.size()))) && (*writer)->finish();
}

constexpr const char *recorder = R"js(
var recording = null, summary = null, player = null;
var heard = {levels: 0, positions: [], ended: false, stopped: null};
function start(file) {
    return omabook.audio.record({file: file, onLevel: function (level) { if (level > 0) heard.levels++; },
                                 onStopped: function (summary) { heard.stopped = summary; }})
        .then(function (made) { recording = made; return {started: made.started, file: made.file}; });
}
function attempt(file) {
    return omabook.audio.record({file: file}).then(function () { return "recording"; }, function (error) { return String(error.message); });
}
function finish() { summary = recording.stop(); return summary; }
function listen() {
    player = omabook.audio.play(summary.file, {onPosition: function (at) { heard.positions.push(at); },
                                                onEnded: function () { heard.ended = true; }});
    return {duration: player.duration, info: omabook.audio.info(summary.file)};
}
function seekTo(at) { player.seek(at); return player.position(); }
function what() { return heard; }
function reach() { return {record: typeof omabook.audio.record, play: typeof omabook.audio.play}; }
)js";

// A stand-in plugin in a folder of its own, with these permissions.
struct Bench {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    Room room;

    std::unique_ptr<ScriptPlugin> plugin(const QStringList &permissions) {
        const QString folder = dir.filePath("plugins/notes");
        QDir().mkpath(folder);
        const QJsonObject manifest{
            {"name", "Notes"}, {"version", "1.0"}, {"script", "main.js"}, {"permissions", QJsonArray::fromStringList(permissions)}};
        Fixtures::write(folder + "/plugin.json", QJsonDocument(manifest).toJson());
        Fixtures::write(folder + "/main.js", recorder);
        ScriptServices services;
        services.settings = &settings;
        services.audio = &room.devices;
        auto made = std::make_unique<ScriptPlugin>(ScriptManifest::read(folder), services);
        QString problem;
        if (!made->load(&problem))
            qWarning() << problem;
        return made;
    }
    QString folder() const { return dir.filePath("plugins/notes"); }
};

// What a function of the script answered, a promise waited for.
[[nodiscard]] QJsonValue answer(ScriptPlugin &plugin, const QString &function, const QJsonArray &arguments = {}) {
    bool done = false;
    QJsonValue result;
    plugin.callFunction(function, arguments, [&](const QJsonValue &value, const QString &problem) {
        result = problem.isEmpty() ? value : QJsonValue(u"problem: "_s + problem);
        done = true;
    });
    for (int i = 0; i < 200 && !done; ++i)
        QTest::qWait(10);
    return result;
}

} // namespace

class AudioTests : public QObject {
    Q_OBJECT

private slots:
    void playingMovesToANewDefaultOutput() {
        Desktop desktop;
        DefaultDevice output(DefaultDevice::Way::Out, std::move(desktop.owned));
        QSignalSpy moved(&output, &DefaultDevice::moved);
        QCOMPARE(output.now().id, "speakers"_ba);

        desktop.devices->setOutput("headphones");
        QCOMPARE(movedTo(moved), QList<QByteArray>{"headphones"});
        QCOMPARE(output.now().id, "headphones"_ba);
        // Another device came, or an input changed: the default is the same, nothing moves.
        desktop.devices->setOutput("headphones");
        desktop.devices->setInput("headset");
        QCOMPARE(moved.size(), 1);
    }

    void aDeviceGoneFallsBackToTheNewDefault() {
        Desktop desktop;
        desktop.devices->setOutput("bluetooth");
        DefaultDevice output(DefaultDevice::Way::Out, std::move(desktop.owned));
        QSignalSpy moved(&output, &DefaultDevice::moved);

        // The headphones are switched off: the desktop's default is the speakers again.
        desktop.devices->setOutput("speakers");
        // None left at all: what plays stays, and goes on once there is one.
        desktop.devices->setOutput({});
        QCOMPARE(output.now().id, QByteArray());
        desktop.devices->setOutput("monitor");
        QCOMPARE(movedTo(moved), (QList<QByteArray>{"speakers", "monitor"}));
    }

    void listeningMovesToANewDefaultInput() {
        Desktop desktop;
        DefaultDevice input(DefaultDevice::Way::In, std::move(desktop.owned));
        QSignalSpy moved(&input, &DefaultDevice::moved);
        QCOMPARE(input.now().id, "built-in"_ba);

        desktop.devices->setOutput("headphones");
        desktop.devices->setInput("headset");
        desktop.devices->setInput("built-in");
        QCOMPARE(movedTo(moved), (QList<QByteArray>{"headset", "built-in"}));
    }

    void aSentenceHeardMovesToTheNewDefault() {
        QTemporaryDir dir;
        const QString wav = dir.filePath("sentence.wav");
        QVERIFY(Fixtures::write(wav, wavOfSilence()));
        Desktop desktop;
        SoundOut sound(std::move(desktop.owned));
        QCOMPARE(sound.device(), "speakers"_ba);

        sound.play(wav);
        desktop.devices->setOutput("headphones");
        QCOMPARE(sound.device(), "headphones"_ba);
        desktop.devices->setOutput("speakers");
        QCOMPARE(sound.device(), "speakers"_ba);
        sound.stop();
    }

    void aRecordingIsReadBackAsItWasWritten() {
        QTemporaryDir dir;
        const QString path = dir.filePath("three.opus");
        const QDateTime started = QDateTime::fromString(u"2026-11-20T09:30:00.250Z"_s, Qt::ISODateWithMs);
        auto writer = Opus::Writer::open({.path = path, .started = started, .bitrate = 32000});
        QVERIFY2(writer, qPrintable(whyNot(writer)));
        const QList<qint16> samples = tone(0, 3, [](int second) { return 4000.0 * (second + 1); });
        // In pieces that are no whole frame, as a microphone gives them.
        for (qsizetype at = 0; at < samples.size(); at += 1234) {
            const qsizetype size = std::min<qsizetype>(1234, samples.size() - at);
            QVERIFY((*writer)->write(std::span<const qint16>(samples.constData() + at, static_cast<std::size_t>(size))));
        }
        QVERIFY((*writer)->finish());
        QVERIFY2((*writer)->bytes() < 3 * 6000, qPrintable(QString::number((*writer)->bytes())));

        auto reader = readerOf(path);
        QVERIFY(reader);
        // To the sample: the encoder's delay and the last frame's padding left out.
        QCOMPARE(reader->length(), 3 * Opus::sampleRate);
        QCOMPARE(reader->duration(), 3000ms);
        QCOMPARE(reader->started(), started);
        QVERIFY(near(loudnessRead(*reader, 0.9), 4000));
        reader->seek(Opus::sampleRate + Opus::sampleRate / 10);
        QVERIFY(near(loudnessRead(*reader, 0.8), 8000));
        reader->seek(2 * Opus::sampleRate);
        QList<qint16> rest(5 * Opus::sampleRate);
        QCOMPARE(reader->read(std::span<qint16>(rest.data(), static_cast<std::size_t>(rest.size()))), qsizetype{Opus::sampleRate});
        QCOMPARE(reader->position(), 3 * Opus::sampleRate);
    }

    void aSeekLandsOnTheMomentAskedFor() {
        QTemporaryDir dir;
        const QString path = dir.filePath("ten.opus");
        QVERIFY(record(path, 10));
        auto reader = readerOf(path);
        QVERIFY(reader);
        // Forward, back, and back again to the start: each second as loud as it was.
        for (const int second : {7, 2, 9, 0, 4}) {
            reader->seek(static_cast<std::int64_t>(second) * Opus::sampleRate + Opus::sampleRate / 10);
            QCOMPARE(reader->position(), static_cast<std::int64_t>(second) * Opus::sampleRate + Opus::sampleRate / 10);
            const double heard = loudnessRead(*reader, 0.8);
            QVERIFY2(near(heard, 2000.0 * (second + 1)), qPrintable(u"second %1: %2"_s.arg(second).arg(heard)));
        }
        reader->seek(20 * Opus::sampleRate);
        QCOMPARE(loudnessRead(*reader, 1), 0.0);
    }

    void aRecordingCutShortIsWholeToItsLastSecond() {
        QTemporaryDir dir;
        const QString path = dir.filePath("cut.opus");
        auto writer = Opus::Writer::open({.path = path, .started = QDateTime::currentDateTimeUtc(), .bitrate = 32000});
        QVERIFY(writer);
        const QList<qint16> samples = tone(0, 5, [](int second) { return 2000.0 * (second + 1); });
        QVERIFY((*writer)->write(std::span<const qint16>(samples.constData(), static_cast<std::size_t>(samples.size()))));
        // The computer goes off now: what reached the disk is all there is,
        // the last page of it half written.
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray onDisk = written.readAll();
        const QString cut = dir.filePath("left.opus");
        QVERIFY(Fixtures::write(cut, onDisk.left(onDisk.size() - 40)));
        auto reader = readerOf(cut);
        QVERIFY(reader);
        QVERIFY2(reader->duration() >= 3000ms && reader->duration() < 5000ms, qPrintable(QString::number(reader->duration().count())));
        reader->seek(2 * Opus::sampleRate + Opus::sampleRate / 10);
        QVERIFY(near(loudnessRead(*reader, 0.8), 6000));
        // A page damaged in the middle is left out, and the rest still read.
        QByteArray damaged = onDisk;
        damaged[damaged.size() / 2] = static_cast<char>(damaged.at(damaged.size() / 2) ^ 0x5a);
        const QString broken = dir.filePath("broken.opus");
        QVERIFY(Fixtures::write(broken, damaged));
        auto readsOn = readerOf(broken);
        QVERIFY(readsOn);
        QList<qint16> all(6 * Opus::sampleRate);
        QVERIFY(readsOn->read(std::span<qint16>(all.data(), static_cast<std::size_t>(all.size()))) >= 2 * Opus::sampleRate);
        QVERIFY(!Opus::Reader::open(dir.filePath("none.opus")));
    }

    void aRecorderPausesAndSaysItsParts() {
        QTemporaryDir dir;
        Room room;
        const QString path = dir.filePath("parts.opus");
        const QDateTime began = QDateTime::fromString(u"2026-11-20T10:00:00.000Z"_s, Qt::ISODateWithMs);
        auto made = AudioRecorder::start(path, room.devices.microphone(), began);
        QVERIFY2(made, qPrintable(whyNot(made)));
        AudioRecorder &recorder = **made;
        QVERIFY(room.microphone && room.microphone->started);
        room.microphone->hear(tone(0, 1, [](int) { return 8000.0; }));
        QCOMPARE(recorder.elapsed(), 1000ms);
        QVERIFY2(recorder.level() > 0.2 && recorder.level() <= 1.0, qPrintable(QString::number(recorder.level())));
        recorder.pause();
        QVERIFY(recorder.paused() && room.microphone->suspended);
        room.microphone->hear(tone(0, 1, [](int) { return 8000.0; }));
        const QDateTime resumed = began.addSecs(90);
        recorder.resume(resumed);
        room.microphone->hear(tone(0, 1, [](int) { return 16000.0; }));
        const AudioRecorder::Summary summary = recorder.stop();
        QVERIFY2(summary.problem.isEmpty(), qPrintable(summary.problem));
        QCOMPARE(summary.duration, 2000ms);
        QCOMPARE(summary.started, began);
        QCOMPARE(summary.parts.size(), 2);
        QCOMPARE(summary.parts.at(1).at, 1000ms);
        QCOMPARE(summary.parts.at(1).started, resumed);
        QCOMPARE(summary.bytes, QFileInfo(path).size());
        QVERIFY(!room.microphone->started);
        auto reader = readerOf(path);
        QVERIFY(reader);
        QCOMPARE(reader->duration(), 2000ms);
        reader->seek(Opus::sampleRate + Opus::sampleRate / 10);
        QVERIFY(near(loudnessRead(*reader, 0.8), 16000));
        // Once stopped it answers the same, and hears nothing more.
        QCOMPARE(recorder.stop().duration, 2000ms);
    }

    void aMicrophoneThatGoesEndsTheRecordingKept() {
        QTemporaryDir dir;
        Room room;
        auto made = AudioRecorder::start(dir.filePath("gone.opus"), room.devices.microphone(), QDateTime::currentDateTimeUtc());
        QVERIFY(made);
        QSignalSpy stopped(made->get(), &AudioRecorder::stopped);
        room.microphone->hear(tone(0, 2, [](int) { return 4000.0; }));
        room.microphone->breakDown();
        QCOMPARE(stopped.size(), 1);
        const AudioRecorder::Summary summary = (*made)->stop();
        QVERIFY(summary.problem.contains(u"unplugged"_s));
        QCOMPARE(summary.duration, 2000ms);
        auto reader = readerOf(dir.filePath("gone.opus"));
        QVERIFY(reader);
        QCOMPARE(reader->duration(), 2000ms);
        // Nowhere to write: no recording, and nothing left behind.
        QVERIFY(!AudioRecorder::start(u"/proc/no/such.opus"_s, room.devices.microphone(), QDateTime::currentDateTimeUtc()));
    }

    void aPlayerSeeksAndSaysWhereItIs() {
        QTemporaryDir dir;
        Room room;
        const QString path = dir.filePath("play.opus");
        QVERIFY(record(path, 4));
        auto made = AudioPlayer::open(path, room.devices.speakers());
        QVERIFY2(made, qPrintable(whyNot(made)));
        AudioPlayer &player = **made;
        QSignalSpy ended(&player, &AudioPlayer::ended);
        QCOMPARE(player.duration(), 4000ms);
        QVERIFY(player.play());
        QVERIFY(player.playing());
        QCOMPARE(room.speakers->take(500ms), qint64{Opus::sampleRate / 2});
        QCOMPARE(player.position(), 500ms);
        player.pause();
        QCOMPARE(room.speakers->take(500ms), qint64{0});
        QCOMPARE(player.position(), 500ms);
        QVERIFY(player.play());
        // Sought while it plays, it plays on from there.
        QVERIFY(player.seek(2100ms));
        QCOMPARE(player.position(), 2100ms);
        QVERIFY(player.playing());
        room.speakers->heard.clear();
        room.speakers->take(800ms);
        QCOMPARE(player.position(), 2900ms);
        QList<qint16> heard(room.speakers->heard.size() / 2);
        std::memcpy(heard.data(), room.speakers->heard.constData(), static_cast<std::size_t>(heard.size()) * 2);
        QVERIFY(near(loudness(std::span<const qint16>(heard.constData(), static_cast<std::size_t>(heard.size()))), 6000));
        room.speakers->take(2000ms);
        QCOMPARE(ended.size(), 1);
        QVERIFY(!player.playing());
        QCOMPARE(player.position(), 4000ms);
        // Sought while it doesn't play, it waits there; played after its end, it starts again.
        QVERIFY(player.seek(1000ms));
        QVERIFY(!player.playing());
        QCOMPARE(player.position(), 1000ms);
        QVERIFY(player.seek(9000ms));
        QCOMPARE(player.position(), 4000ms);
        QVERIFY(player.play());
        QCOMPARE(player.position(), 0ms);
        QVERIFY(!AudioPlayer::open(dir.filePath("none.opus"), room.devices.speakers()));
    }

    void twoHoursStaySound() {
        QTemporaryDir dir;
        Room room;
        const QString path = dir.filePath("long.opus");
        QElapsedTimer took;
        took.start();
        auto made = AudioRecorder::start(path, room.devices.microphone(), QDateTime::currentDateTimeUtc());
        QVERIFY(made);
        // Each minute as loud as its own: 1000 and 200 more a minute, round to 13000.
        const auto minute = [](int second) { return 1000.0 + 200.0 * ((second / 60) % 60); };
        constexpr int hours = 2, piece = 10;
        for (int second = 0; second < hours * 3600; second += piece)
            room.microphone->hear(tone(second, piece, minute));
        const AudioRecorder::Summary summary = (*made)->stop();
        qInfo() << "two hours recorded in" << took.elapsed() << "ms," << summary.bytes << "bytes";
        QVERIFY2(summary.problem.isEmpty(), qPrintable(summary.problem));
        QCOMPARE(summary.duration, std::chrono::milliseconds(hours * 3600 * 1000));
        QVERIFY2(summary.bytes < 40'000'000, qPrintable(QString::number(summary.bytes)));
        auto reader = readerOf(path);
        QVERIFY(reader);
        QCOMPARE(reader->length(), std::int64_t{hours} * 3600 * Opus::sampleRate);
        // The end is as loud as it was, and so is a minute in the middle.
        for (const int second : {2 * 3600 - 30, 3600 + 7 * 60 + 20, 5}) {
            reader->seek(std::int64_t{second} * Opus::sampleRate);
            const double heard = loudnessRead(*reader, 1);
            QVERIFY2(near(heard, minute(second)), qPrintable(u"at %1 s: %2"_s.arg(second).arg(heard)));
        }
    }

    void aPluginRecordsWithTheYesAndPlaysItBack() {
        Bench bench;
        auto plugin = bench.plugin({u"microphone"_s});
        QList<QJsonObject> asked;
        connect(plugin.get(), &ScriptPlugin::formRequested, plugin.get(), [&](int token, const QJsonObject &form) {
            asked << form;
            plugin->answerForm(token, QJsonObject());
        });
        const QJsonValue started = answer(*plugin, u"start"_s, {u"data/recordings/one.opus"_s});
        QVERIFY2(started.isObject(), qPrintable(started.toString()));
        QCOMPARE(started["file"].toString(), u"data/recordings/one.opus"_s);
        QVERIFY(QDateTime::fromString(started["started"].toString(), Qt::ISODateWithMs).secsTo(QDateTime::currentDateTimeUtc()) < 60);
        // The person was asked, in the window's words, and the window shows it recording.
        QCOMPARE(asked.size(), 1);
        QVERIFY(asked.first()["title"].toString().contains(u"Notes"_s));
        QVERIFY(bench.room.devices.recording());
        QCOMPARE(bench.room.devices.recorder(), u"Notes"_s);
        bench.room.microphone->hear(tone(0, 2, [](int) { return 9000.0; }));
        QTRY_VERIFY(answer(*plugin, u"what"_s)["levels"].toInt() > 0);
        QVERIFY(bench.room.devices.level() > 0);
        QCOMPARE(bench.room.devices.elapsed(), 2000.0);

        const QJsonValue summary = answer(*plugin, u"finish"_s);
        QCOMPARE(summary["duration"].toInt(), 2000);
        QCOMPARE(summary["file"].toString(), u"data/recordings/one.opus"_s);
        QCOMPARE(summary["parts"].toArray().size(), 1);
        QVERIFY(!bench.room.devices.recording());
        QVERIFY(QFile::exists(bench.folder() + "/data/recordings/one.opus"));
        // A second recording is not asked about again.
        QVERIFY(answer(*plugin, u"start"_s, {u"data/recordings/two.opus"_s}).isObject());
        QCOMPARE(asked.size(), 1);
        QVERIFY(answer(*plugin, u"finish"_s).isObject());

        const QJsonValue opened = answer(*plugin, u"listen"_s);
        QCOMPARE(opened["duration"].toInt(), 0); // the second, which heard nothing
        QCOMPARE(opened["info"]["duration"].toInt(), 0);
    }

    void aPluginPlaysWhatItRecordedFromAnywhere() {
        Bench bench;
        auto plugin = bench.plugin({u"microphone"_s});
        connect(plugin.get(), &ScriptPlugin::formRequested, plugin.get(),
                [&](int token, const QJsonObject &) { plugin->answerForm(token, QJsonObject()); });
        QVERIFY(answer(*plugin, u"start"_s, {u"data/one.opus"_s}).isObject());
        bench.room.microphone->hear(tone(0, 3, [](int second) { return 3000.0 * (second + 1); }));
        QVERIFY(answer(*plugin, u"finish"_s).isObject());
        const QJsonValue opened = answer(*plugin, u"listen"_s);
        QCOMPARE(opened["duration"].toInt(), 3000);
        QVERIFY(bench.room.speakers);
        bench.room.speakers->take(500ms);
        QTRY_VERIFY(answer(*plugin, u"what"_s)["positions"].toArray().contains(500));
        QCOMPARE(answer(*plugin, u"seekTo"_s, {2000}).toInt(), 2000);
        bench.room.speakers->take(1500ms);
        QTRY_VERIFY(answer(*plugin, u"what"_s)["ended"].toBool());
        QCOMPARE(answer(*plugin, u"what"_s)["positions"].toArray().last().toInt(), 3000);
    }

    void nothingIsRecordedWithoutAYes() {
        Bench bench;
        auto plugin = bench.plugin({u"microphone"_s});
        // Where nobody can be asked, as in a command.
        QVERIFY(answer(*plugin, u"attempt"_s, {u"data/a.opus"_s}).toString().contains(u"Nobody is here"_s));
        connect(plugin.get(), &ScriptPlugin::formRequested, plugin.get(),
                [&](int token, const QJsonObject &) { plugin->answerForm(token, QJsonValue()); });
        QVERIFY(answer(*plugin, u"attempt"_s, {u"data/a.opus"_s}).toString().contains(u"didn't let"_s));
        QVERIFY(!QFile::exists(bench.folder() + "/data/a.opus"));
        QVERIFY(!bench.room.microphone);
        // Only under data/, and only as Opus.
        QVERIFY(answer(*plugin, u"attempt"_s, {u"main.opus"_s}).toString().startsWith(u"problem: "_s));
        QVERIFY(answer(*plugin, u"attempt"_s, {u"data/a.wav"_s}).toString().contains(u".opus"_s));
        QVERIFY(answer(*plugin, u"attempt"_s, {u"../elsewhere/a.opus"_s}).toString().startsWith(u"problem: "_s));
        // Without the permission there is no recording at all, only playing.
        auto plain = bench.plugin({});
        const QJsonValue reach = answer(*plain, u"reach"_s);
        QCOMPARE(reach["record"].toString(), u"undefined"_s);
        QCOMPARE(reach["play"].toString(), u"function"_s);
        QVERIFY(ScriptApi::permissionNames().contains(u"microphone"_s));
    }

    void theWindowStopsARecordingAndThePluginIsTold() {
        Bench bench;
        auto plugin = bench.plugin({u"microphone"_s});
        connect(plugin.get(), &ScriptPlugin::formRequested, plugin.get(),
                [&](int token, const QJsonObject &) { plugin->answerForm(token, QJsonObject()); });
        QVERIFY(answer(*plugin, u"start"_s, {u"data/stopped.opus"_s}).isObject());
        bench.room.microphone->hear(tone(0, 1, [](int) { return 5000.0; }));
        QSignalSpy shown(&bench.room.devices, &AudioDevices::recordingChanged);
        bench.room.devices.stopRecording();
        QCOMPARE(shown.size(), 1);
        QVERIFY(!bench.room.devices.recording());
        const QJsonValue stopped = answer(*plugin, u"what"_s)["stopped"];
        QCOMPARE(stopped["duration"].toInt(), 1000);
        QCOMPARE(stopped["file"].toString(), u"data/stopped.opus"_s);
        // A plugin that goes while it records keeps what it recorded, and the mark goes with it.
        QVERIFY(answer(*plugin, u"start"_s, {u"data/left.opus"_s}).isObject());
        bench.room.microphone->hear(tone(0, 1, [](int) { return 5000.0; }));
        QVERIFY(bench.room.devices.recording());
        plugin.reset();
        QVERIFY(!bench.room.devices.recording());
        auto reader = readerOf(bench.folder() + "/data/left.opus");
        QVERIFY(reader);
        QCOMPARE(reader->duration(), 1000ms);
    }
};

QTEST_MAIN(AudioTests)
#include "audio_tests.moc"
