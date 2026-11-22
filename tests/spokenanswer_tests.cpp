// Asking the AI by voice and hearing the answer (#149): the reading
// companion and "ask about your library" asked by the push-to-talk key, a
// mouse, a finger and a pen; the question asked as it was heard, never with
// more of the book than has been read; the answer shown and read aloud, a
// sentence at a time, named in the bar over the page, stopped by the bar,
// Shift+R, Escape or asking again; none of it with the AI Off. Both engines
// stand in (voxtype for speech to text, a voice that writes down what it is
// given for text to speech), and so does the model.

#include <QDir>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include "desk.h"
#include "dictation.h"
#include "fixtures.h"
#include "spokenanswer.h"
#include "testaudio.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure;

const QPointingDevice &pen() {
    static QPointingDevice device{"pen", 8812, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&device);
        return true;
    }();
    return device;
}

// voxtype, standing in: it writes out any WAV as one question.
[[nodiscard]] QString standInVoxtype(const QString &folder) {
    return Fixtures::script(folder + "/voxtype", R"sh(
case "$*" in
  *transcribe*)
    echo "Loading audio file"
    echo
    echo "Who waits at the rail?"
    ;;
esac
)sh");
}

// A voice that writes down each sentence it is given, and reads it at once
// or (`holding`) only when told to finish.
class ListeningVoice : public Voice {
public:
    explicit ListeningVoice(QStringList *said, int *stops, bool holding) : m_said(said), m_stops(stops), m_holding(holding) {}
    void say(const QString &sentence, const Speech &) override {
        *m_said << sentence;
        const int turn = ++m_turn;
        if (!m_holding)
            QTimer::singleShot(0, this, [this, turn] {
                if (turn == m_turn)
                    emit spoken();
            });
    }
    void stop() override {
        ++*m_stops;
        ++m_turn;
    }
    [[nodiscard]] QString name() const override { return u"Stand-in"_s; }

private:
    QStringList *m_said;
    int *m_stops;
    bool m_holding;
    int m_turn = 0;
};

// What the voices were given, across them: each answer gets a voice of its own.
struct Ears {
    QStringList said;
    int stops = 0;
    bool holding = false;
    int made = 0;

    SpokenAnswer::VoiceMaker maker() {
        return [this]() -> std::expected<std::unique_ptr<Voice>, Failure> {
            ++made;
            return std::make_unique<ListeningVoice>(&said, &stops, holding);
        };
    }
};

// A model that answers in Markdown, two sentences, and keeps what it was asked.
class AnsweringAi : public AiProvider {
public:
    explicit AnsweringAi(QList<AiRequest> *asked) : m_asked(asked) {}
    QString name() const override { return "Testbot"; }
    void ask(const AiRequest &request) override {
        *m_asked << request;
        QTimer::singleShot(0, this, [this] {
            emit delta("**Mira** waits at the rail. ");
            emit delta("She watches the [far pier](https://example.org).");
            emit finished();
        });
    }
    void cancel() override {}

private:
    QList<AiRequest> *m_asked;
};

[[nodiscard]] QString textOf(QQuickItem *item) {
    QString words;
    if (item)
        QMetaObject::invokeMethod(item, "getText", Q_RETURN_ARG(QString, words), Q_ARG(int, 0),
                                  Q_ARG(int, item->property("length").toInt()));
    return words;
}
[[nodiscard]] QPoint middle(const QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }
[[nodiscard]] QString everything(const AiRequest &request) {
    QString all = request.system;
    for (const AiMessage &message : request.messages)
        all += '\n' + message.text;
    return all;
}

// The window with a book of two chapters open in the first, voxtype and a
// voice standing in, a microphone that hears, and the model above.
struct Bench {
    Desk desk;
    qint64 book = 0;
    QList<AiRequest> asked;
    Ears ears;
    ulong clock = 1000;

    explicit Bench(const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false) : desk(language, colors, touch) {
        desk.settings->setRecognition({{"voxtype_program", standInVoxtype(desk.dir.path())}});
        desk.backend->audio()->setInputs(*TestAudio::microphone("tone"));
        desk.backend->spokenAnswer()->setVoiceMaker(ears.maker());
        desk.assistant.emplace(&*desk.reader,
                               [this](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<AnsweringAi>(&asked); });
        const QString file = desk.dir.filePath("The Ferry.epub");
        QVERIFY(Fixtures::chapteredEpub(
            file, "The Ferry",
            {{"One", "<h1>One</h1><p>It was late in the autumn when the ferry came back to the island.</p>"
                     "<p>Mira stood at the rail and watched the far pier.</p>"},
             {"Two", "<h1>Two</h1><p>The lantern keeper was her father all along, hidden in the lighthouse.</p>"}}));
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1280, 820)));
        desk.quicken();
    }

    [[nodiscard]] Dictation &dictation() { return *desk.backend->dictation(); }
    [[nodiscard]] SpokenAnswer &spoken() { return *desk.backend->spokenAnswer(); }
    [[nodiscard]] QQuickItem *onScreen(const QString &name) const {
        QList<QQuickItem *> items{desk.window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (item->objectName() == name && item->isVisible() && item->width() > 0)
                return item;
            items << item->childItems();
        }
        return nullptr;
    }
    [[nodiscard]] QQuickItem *field() const { return desk.window->activeFocusItem(); }

    // The book open, and the companion beside it with the keyboard in its question.
    bool companion() {
        desk.reader->openBook(book);
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && desk.shows("touchArea"); }, 5000))
            return false;
        desk.settle();
        if (!desk.run("reader.ask"))
            return false;
        return QTest::qWaitFor([&] { return onScreen("askByVoice") && field() && field()->objectName() == "question"; }, 3000);
    }
    void holdKey(int ms = 600) const {
        QTest::keyPress(desk.window, Qt::Key_M, Qt::ControlModifier);
        QTest::qWait(ms);
        QTest::keyRelease(desk.window, Qt::Key_M, Qt::ControlModifier);
    }
    void penAt(QPoint at, bool down) {
        clock += 5;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    [[nodiscard]] QString speaker() const {
        QObject *label = desk.find("aloudSpeaker");
        return label && qobject_cast<QQuickItem *>(label)->isVisible() ? label->property("text").toString() : QString();
    }
    [[nodiscard]] QString lastAnswer() const {
        const QVariantList messages = desk.assistant->messages();
        return messages.isEmpty() ? QString() : messages.last().toMap().value("text").toString();
    }
};

const QStringList answerRead{u"Mira waits at the rail."_s, u"She watches the far pier."_s};

} // namespace

class SpokenAnswerTests : public QObject {
    Q_OBJECT

    QStringList found;
    int looked = 0;

    void look(Desk &desk, const QString &state) {
        desk.settle(1);
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        ++looked;
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        for (const QString &problem : std::as_const(survey.problems))
            if (!found.contains(where + " — " + problem))
                found << where + " — " + problem;
        desk.shot(QString("spoken-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }
    void report() {
        QVERIFY(looked > 0);
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // An answer's words as a voice reads them: no Markdown marks, no addresses.
    void anAnswerIsReadWithoutItsMarkdown() {
        QCOMPARE(SpokenAnswer::plain(u"**Mira** waits at the [rail](https://example.org).\n\n# Then\n\n- She *watches*."_s),
                 u"Mira waits at the rail.\nThen\nShe watches."_s);
        QCOMPARE(SpokenAnswer::plain(u"  "_s), QString());
    }

    // Read a sentence at a time, named while it reads, stopped by the
    // window's stop as a plugin's reading is, and by a new answer.
    void anAnswerIsReadAndStopped() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        AudioDevices devices;
        SpokenAnswer spoken(&settings, &devices);
        Ears ears;
        spoken.setVoiceMaker(ears.maker());

        spoken.say(u"One thing. And another."_s, u"Testbot"_s, u"en"_s);
        QVERIFY(spoken.speaking());
        QVERIFY(devices.speaking());
        QCOMPARE(devices.speaker(), u"Testbot"_s);
        QTRY_VERIFY(!spoken.speaking());
        QCOMPARE(ears.said, (QStringList{u"One thing."_s, u"And another."_s}));
        QVERIFY(!devices.speaking());

        ears.holding = true;
        spoken.say(u"Held."_s, u"Testbot"_s, u"en"_s);
        QVERIFY(spoken.speaking());
        devices.stopSpeaking();
        QVERIFY(!spoken.speaking());
        QVERIFY(!devices.speaking());
        QVERIFY(ears.stops > 0);

        // A new answer stops the one before it.
        spoken.say(u"First."_s, u"Testbot"_s, u"en"_s);
        const int stopsBefore = ears.stops;
        spoken.say(u"Second."_s, u"Testbot"_s, u"en"_s);
        QVERIFY(ears.stops > stopsBefore);
        QCOMPARE(ears.said.last(), u"Second."_s);
        spoken.stop();
        QVERIFY(!devices.speaking());

        // No voice: it says why, and nothing reads.
        spoken.setVoiceMaker(
            []() -> std::expected<std::unique_ptr<Voice>, Failure> { return std::unexpected(Failure(Failure::Code::PiperMissing)); });
        QSignalSpy failed(&spoken, &SpokenAnswer::failed);
        spoken.say(u"Nothing."_s, u"Testbot"_s, u"en"_s);
        QCOMPARE(failed.size(), 1);
        QVERIFY(!failed.first().first().toString().isEmpty());
        QVERIFY(!spoken.speaking());
    }

    // The companion asked by holding the key: the question as it was heard,
    // with the book only as far as it is read, and the answer shown and read
    // aloud, the bar over the page naming the AI; Shift+R stops it.
    void theCompanionIsAskedByVoiceAndAnswersAloud() {
        Bench bench;
        QVERIFY(bench.companion());
        bench.holdKey();
        QTRY_COMPARE(bench.asked.size(), 1);
        QVERIFY(everything(bench.asked.first()).contains("Who waits at the rail?"));
        QVERIFY(everything(bench.asked.first()).contains("Mira stood at the rail"));
        // Never past where the reader has read.
        QVERIFY(!everything(bench.asked.first()).contains("her father all along"));
        QTRY_COMPARE(bench.ears.said, answerRead);
        QVERIFY(bench.lastAnswer().contains("**Mira** waits"));
        QCOMPARE(textOf(bench.desk.item("question")), QString());
        QTRY_VERIFY(!bench.spoken().speaking());

        // Held this time: the bar names who reads, and Shift+R stops it.
        bench.ears.holding = true;
        bench.ears.said.clear();
        bench.holdKey();
        QTRY_VERIFY(bench.spoken().speaking());
        QTRY_COMPARE(bench.speaker(), u"Testbot is reading aloud"_s);
        QCOMPARE(bench.ears.said.first(), answerRead.first());
        // In the question Shift+R is a letter; on the page it stops the answer.
        bench.desk.item("readerView")->forceActiveFocus();
        QTest::keyClick(bench.desk.window, Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY(!bench.spoken().speaking());
        QVERIFY(!bench.desk.backend->audio()->speaking());
        // And the bar's ✕.
        bench.desk.item("question")->forceActiveFocus();
        bench.holdKey();
        QTRY_VERIFY(bench.onScreen("aloudStop"));
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(bench.onScreen("aloudStop")));
        QTRY_VERIFY(!bench.spoken().speaking());
        QCOMPARE(bench.asked.size(), 3);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // While an answer is read, asking again stops it: by the key, a click,
    // a finger and a pen on the microphone; Escape stops it too, and the
    // button reads the last answer again, or stops it.
    void askingAgainStopsTheAnswer() {
        Bench bench;
        bench.ears.holding = true;
        QVERIFY(bench.companion());
        bench.holdKey();
        QTRY_VERIFY(bench.spoken().speaking());

        // The key again: it stops at once, and the next answer is read.
        QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.dictation().listening());
        QVERIFY(!bench.spoken().speaking());
        QTest::qWait(500);
        QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_COMPARE(bench.asked.size(), 2);
        QTRY_VERIFY(bench.spoken().speaking());

        // A click to start, while it reads, and one to stop.
        QQuickItem *mic = bench.onScreen("askByVoice");
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(mic));
        QTRY_VERIFY(bench.dictation().listening());
        QVERIFY(!bench.spoken().speaking());
        QTest::qWait(600);
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(mic));
        QTRY_COMPARE(bench.asked.size(), 3);
        QTRY_VERIFY(bench.spoken().speaking());

        // A finger held on it.
        static QPointingDevice *const finger = QTest::createTouchDevice();
        QTest::touchEvent(bench.desk.window, finger).press(0, middle(mic), bench.desk.window);
        QTRY_VERIFY(bench.dictation().listening());
        QVERIFY(!bench.spoken().speaking());
        QTest::qWait(600);
        QTest::touchEvent(bench.desk.window, finger).release(0, middle(mic), bench.desk.window);
        QTRY_COMPARE(bench.asked.size(), 4);
        QTRY_VERIFY(bench.spoken().speaking());

        // A pen held on it.
        bench.penAt(middle(mic), true);
        QTRY_VERIFY(bench.dictation().listening());
        QVERIFY(!bench.spoken().speaking());
        QTest::qWait(600);
        bench.penAt(middle(mic), false);
        QTRY_COMPARE(bench.asked.size(), 5);
        QTRY_VERIFY(bench.spoken().speaking());

        // Escape stops it, and the companion stays open; the next closes it.
        bench.desk.item("question")->forceActiveFocus();
        QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        QVERIFY(!bench.spoken().speaking());
        QVERIFY(bench.onScreen("assistantPane"));

        // The button reads the last answer again, and stops it.
        QTRY_VERIFY(bench.onScreen("answerAloud"));
        bench.ears.said.clear();
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(bench.onScreen("answerAloud")));
        QTRY_VERIFY(bench.spoken().speaking());
        QCOMPARE(bench.ears.said, QStringList{answerRead.first()});
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(bench.onScreen("answerAloud")));
        QVERIFY(!bench.spoken().speaking());
        // A typed question's answer is shown, not read.
        bench.ears.said.clear();
        bench.desk.item("question")->forceActiveFocus();
        bench.desk.item("question")->setProperty("text", u"And then?"_s);
        QTest::keyClick(bench.desk.window, Qt::Key_Return);
        QTRY_COMPARE(bench.asked.size(), 6);
        QTRY_VERIFY(!bench.desk.assistant->busy());
        QTest::qWait(200);
        QVERIFY(bench.ears.said.isEmpty());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // A book read aloud waits while the companion is asked and answers.
    void theBooksReadingWaitsForTheQuestion() {
        Bench bench;
        QVERIFY(readingAloud(bench.desk, bench.book));
        QVERIFY(bench.desk.reader->readAloud()->speaking());
        QVERIFY(bench.desk.run("reader.ask"));
        QTRY_VERIFY(bench.field() && bench.field()->objectName() == "question");
        bench.holdKey();
        QVERIFY(!bench.desk.reader->readAloud()->speaking());
        QTRY_COMPARE(bench.ears.said, answerRead);
    }

    // "Ask about your library" by voice: the key in its line, the answer
    // shown and read aloud, and its button stops it.
    void theLibraryIsAskedByVoice() {
        Bench bench;
        bench.ears.holding = true;
        bench.desk.call("showAskLibrary");
        QVERIFY(bench.desk.waitForPopup());
        QTRY_VERIFY(bench.onScreen("askLibraryVoice"));
        QTRY_COMPARE(bench.field()->objectName(), u"askLibraryWords"_s);
        bench.holdKey();
        QTRY_VERIFY(bench.spoken().speaking());
        QVERIFY(!bench.desk.librarian->answer().isEmpty());
        QVERIFY(bench.desk.librarian->answer().contains(bench.ears.said.first().left(12)));
        QTRY_VERIFY(bench.onScreen("askLibraryAloud"));
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(bench.onScreen("askLibraryAloud")));
        QVERIFY(!bench.spoken().speaking());
        // By a finger, under fingers.
        static QPointingDevice *const finger = QTest::createTouchDevice();
        QQuickItem *mic = bench.onScreen("askLibraryVoice");
        QTest::touchEvent(bench.desk.window, finger).press(0, middle(mic), bench.desk.window);
        QTRY_VERIFY2(bench.dictation().listening(), qPrintable(QString("mic at %1,%2 %3x%4, available %5")
                                                                   .arg(middle(mic).x())
                                                                   .arg(middle(mic).y())
                                                                   .arg(mic->width())
                                                                   .arg(mic->height())
                                                                   .arg(mic->property("available").toBool())));
        QTest::qWait(600);
        QTest::touchEvent(bench.desk.window, finger).release(0, middle(mic), bench.desk.window);
        QTRY_VERIFY(bench.spoken().speaking());
        bench.spoken().stop();
        // Changing Omabook takes no voice.
        QVERIFY(bench.desk.closePopups());
        QVERIFY(bench.desk.run("global.change"));
        QVERIFY(bench.desk.waitForPopup());
        QVERIFY(!bench.onScreen("changeItVoice"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // With the AI Off there is no microphone to ask with, and the key asks nothing.
    void nothingIsAskedByVoiceWithTheAiOff() {
        Bench bench;
        bench.desk.settings->setAiProvider("off");
        QVERIFY(!bench.companion());
        QVERIFY(!bench.onScreen("askByVoice"));
        bench.desk.item("question")->forceActiveFocus();
        bench.holdKey();
        QVERIFY(!bench.dictation().listening());
        QTest::qWait(300);
        QVERIFY(bench.asked.isEmpty());
        QVERIFY(bench.ears.said.isEmpty());
        QVERIFY(bench.desk.closePopups());
        bench.desk.call("showAskLibrary");
        QVERIFY(bench.desk.waitForPopup());
        QVERIFY(!bench.onScreen("askLibraryVoice"));
    }

    // The companion with its microphone, listening and reading aloud, and
    // the library's question, at every size, in English, German and a
    // right-to-left language, and under fingers.
    void askingByVoiceAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Bench bench(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            bench.ears.holding = true;
            if (bench.desk.window->minimumWidth() > 420 || bench.desk.window->minimumHeight() > 360)
                bench.desk.window->setMinimumSize(QSize(420, 360));
            for (const QSize &size : sizes()) {
                QVERIFY(bench.desk.resize(size));
                if (!bench.onScreen("askByVoice"))
                    QVERIFY(bench.companion());
                bench.desk.item("question")->forceActiveFocus();
                look(bench.desk, "companion");
                QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
                QTRY_VERIFY(bench.dictation().listening());
                QTest::qWait(400);
                look(bench.desk, "listening");
                QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
                QTRY_VERIFY(bench.spoken().speaking());
                look(bench.desk, "answering");
                bench.spoken().stop();
            }
            bench.desk.reader->close();
            for (const QSize &size : dialogSizes()) {
                QVERIFY(bench.desk.resize(size));
                bench.desk.call("showAskLibrary");
                QVERIFY(bench.desk.waitForPopup());
                QTRY_VERIFY(bench.onScreen("askLibraryVoice"));
                bench.desk.item("askLibraryWords")->forceActiveFocus();
                bench.holdKey(400);
                QTRY_VERIFY(bench.spoken().speaking());
                look(bench.desk, "library");
                bench.spoken().stop();
                QVERIFY(bench.desk.closePopups());
            }
            QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
        }
        report();
    }
};

QTEST_MAIN(SpokenAnswerTests)
#include "spokenanswer_tests.moc"
