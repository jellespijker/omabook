// Writing out a recording of the library from the window (#151): its
// details say where it stands and write it out, by the key or a tap; the
// mark at the top of the window says how far it is and stops it, and
// started again it goes on from there. Looked over at every size, in a long
// language, a language that reads right to left and under fingers, with
// nothing cut off or overlapping (the layout survey). voxtype is a stand-in
// that writes out a piece only once it is let to, so each state holds still.

#include <QFile>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <array>

#include "desk.h"
#include "dictation.h"
#include "testaudio.h"
#include "transcriptdesk.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// Silence, `seconds` long, as a WAV of 16 kHz mono.
[[nodiscard]] bool silence(const QString &path, int seconds) {
    const auto le = [](quint32 value, int bytes) {
        std::array<char, 4> four{};
        qToLittleEndian(value, four.data());
        return QByteArray(four.data(), bytes);
    };
    const QByteArray data(qsizetype{32000} * seconds, '\0');
    return Fixtures::write(path, "RIFF" + le(36 + data.size(), 4) + "WAVEfmt " + le(16, 4) + le(1, 2) + le(1, 2) + le(16000, 4)
                                     + le(32000, 4) + le(2, 2) + le(16, 2) + "data" + le(data.size(), 4) + data);
}

// voxtype, standing in: the n-th piece is written out once the file go-n is there.
[[nodiscard]] QString heldVoxtype(const QString &folder) {
    return Fixtures::script(folder + "/voxtype", R"sh(
here="$(dirname "$0")"
echo x >> "$here/calls"
n=$(wc -l < "$here/calls")
while [ ! -e "$here/go-$n" ]; do sleep 0.05; done
echo
echo "Spoken words of part $n."
)sh");
}

} // namespace

class TranscriptViewTests : public QObject {
    Q_OBJECT

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
        desk.shot(QString("transcript-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

    static QString text(const Desk &desk, const QString &name) {
        QObject *found = desk.find(name);
        return found ? found->property("text").toString() : QString();
    }
    static void let(const Desk &desk, int piece) { QVERIFY(Fixtures::write(desk.dir.filePath(u"bin/go-%1"_s.arg(piece)), "go")); }
    // How many pieces voxtype was asked for.
    static int asked(const Desk &desk) {
        QFile calls(desk.dir.filePath("bin/calls"));
        return calls.open(QIODevice::ReadOnly) ? static_cast<int>(calls.readAll().count('\n')) : 0;
    }

    // A recording on the shelf, selected, its details shown, and voxtype held.
    static qint64 recording(Desk &desk) {
        QDir().mkpath(desk.dir.filePath("bin"));
        desk.settings->setRecognition({{"voxtype_program", heldVoxtype(desk.dir.filePath("bin"))}});
        desk.backend->transcripts()->transcriber()->setPieceLength(1s);
        const QString file =
            desk.dir.filePath("Vorlesung über die Wärmebehandlung von Stählen, dritter Teil - Wilhelmina Konstantinopolitanska.wav");
        if (!silence(file, 4))
            return 0;
        const qint64 id = desk.backend->library()->add(file).id;
        desk.backend->reload();
        return id;
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The key writes it out, the mark stops it, and the key again goes on from there.
    void aRecordingIsWrittenOutStoppedAndGoneOnWith() {
        // Where the transcripts are kept: a folder of this test's own, before the window's is made.
        const QTemporaryDir data;
        qputenv("XDG_DATA_HOME", data.path().toUtf8());
        Desk desk("en", {}, false);
        const qint64 id = recording(desk);
        QVERIFY(id > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize({1180, 780}));
        desk.call("selectOnShelf", id);
        QTRY_VERIFY(desk.shows("detailsRecording"));
        QVERIFY2(text(desk, "detailsRecordingState").startsWith("Not written out yet."), qPrintable(text(desk, "detailsRecordingState")));
        QCOMPARE(text(desk, "transcribeButton"), u"Write Out"_s);

        QObject *holder = desk.find("transcribingHolder");
        QVERIFY(holder);
        QTRY_VERIFY2(holder->property("recording").toBool(),
                     qPrintable(holder->property("selected").toString() + " " + holder->parent()->metaObject()->className()));
        QVERIFY(desk.run("library.transcribe"));
        QTRY_VERIFY(desk.shows("transcribingMark"));
        QTRY_COMPARE(text(desk, "transcribingStatus"), u"Writing out “Vorlesung über die Wärmebehandlung von Stählen, dritter Teil”…"_s);
        QTRY_COMPARE(text(desk, "transcribingCount"), u"0 of 4"_s);
        QCOMPARE(text(desk, "transcribeButton"), u"Stop Writing Out"_s);
        let(desk, 1);
        QTRY_COMPARE(text(desk, "transcribingCount"), u"1 of 4"_s);
        // The second piece is under way when it is stopped.
        QTRY_COMPARE(asked(desk), 2);
        QVERIFY(QMetaObject::invokeMethod(desk.find("stopTranscribing"), "clicked"));
        QTRY_VERIFY(!desk.shows("transcribingMark"));
        QTRY_COMPARE(text(desk, "detailsRecordingState"), u"Written out in part, 1 of 4. It goes on from there."_s);
        QCOMPARE(text(desk, "transcribeButton"), u"Go On Writing Out"_s);

        for (int piece = 2; piece <= 5; ++piece)
            let(desk, piece);
        QVERIFY(desk.run("library.transcribe"));
        QTRY_VERIFY_WITH_TIMEOUT(!desk.backend->transcripts()->busy(), 20000);
        QTRY_VERIFY(text(desk, "detailsRecordingState").startsWith("Written out by the sentence"));
        QVERIFY(!desk.shows("transcribeButton"));
        // Each piece was asked for once, and the second again, as it was under way when it was stopped.
        QCOMPARE(asked(desk), 5);
        QVERIFY(desk.warnings.isEmpty());
    }

    // With the AI Off, it says why it can't be written out, and nothing starts.
    void withTheAiOffItSaysWhy() {
        const QTemporaryDir data;
        qputenv("XDG_DATA_HOME", data.path().toUtf8());
        Desk desk("en", {}, false);
        const qint64 id = recording(desk);
        desk.settings->setAiProvider("off");
        QVERIFY(desk.show());
        QVERIFY(desk.resize({1180, 780}));
        desk.call("selectOnShelf", id);
        QTRY_VERIFY(desk.shows("detailsRecording"));
        QVERIFY(!desk.find("transcribeButton")->property("enabled").toBool());
        QVERIFY2(!text(desk, "detailsRecordingState").startsWith("Not written out"), qPrintable(text(desk, "detailsRecordingState")));
    }

    // The details and the mark at every size, in German, under fingers and right to left.
    void theDetailsAndTheMarkAtEverySize() {
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            const QTemporaryDir data;
            qputenv("XDG_DATA_HOME", data.path().toUtf8());
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            const qint64 id = recording(desk);
            QVERIFY(id > 0);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            desk.call("selectOnShelf", id);
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "untranscribed");
            }
            desk.backend->transcripts()->toggle(id);
            QTRY_VERIFY(desk.shows("transcribingMark"));
            let(desk, 1);
            QTRY_COMPARE(desk.backend->transcripts()->done(), 1);
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "writing");
            }
            // Dictation listening (#148) at the same time: its mark and this one, one under the other.
            desk.backend->audio()->setInputs(*TestAudio::microphone("tone"));
            QVERIFY(desk.backend->dictation()->listen({}, false) > 0);
            QTRY_VERIFY(desk.backend->dictation()->listening());
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                QVERIFY(desk.shows("recordingMark") && desk.shows("transcribingMark"));
                const auto rect = [&desk](const char *name) {
                    QQuickItem *item = desk.item(name);
                    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                };
                QVERIFY2(!rect("recordingMark").intersects(rect("transcribingMark")),
                         qPrintable(QString("%1x%2").arg(size.width()).arg(size.height())));
                look(desk, "listening");
            }
            desk.backend->dictation()->cancel();
            QTRY_VERIFY(!desk.shows("recordingMark"));
            desk.backend->transcripts()->stop();
            QTRY_VERIFY(!desk.backend->transcripts()->busy());
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "partly");
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = std::exchange(found, {});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }
};

QTEST_MAIN(TranscriptViewTests)
#include "transcriptview_tests.moc"
