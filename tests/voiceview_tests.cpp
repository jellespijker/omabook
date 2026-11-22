// Commands said while reading, in the window (#150): from a transcript as
// speech to text writes it, in each of the five languages, by the key held
// while speaking, and by the bar's Speak with a mouse, a finger and a pen;
// the mark at the top saying all along that it listens; Escape letting go;
// what isn't understood said plainly, after the model was asked; nothing at
// all with the AI Off; and the bar and the mark at every size, in English,
// German, a right-to-left language and under fingers. The engine is a voxtype
// standing in that hears what the test wrote down, the microphone a test one.

#include <QFile>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QTimer>
#include <QtTest>
#include <expected>
#include <qpa/qwindowsysteminterface.h>

#include "desk.h"
#include "dictation.h"
#include "fixtures.h"
#include "readaloud.h"
#include "testaudio.h"
#include "voice.h"
#include "voicecommands.h"

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

// voxtype, standing in: it writes out any WAV as what the test wrote in
// `said` beside it, slowly while `slow` is there too.
[[nodiscard]] QString standInVoxtype(const QString &folder) {
    return Fixtures::script(folder + "/voxtype", R"sh(
here="$(dirname "$0")"
case "$*" in
  *transcribe*)
    if [ -f "$here/slow" ]; then sleep 3; fi
    echo "Loading audio file"
    echo
    cat "$here/said"
    ;;
esac
)sh");
}

// A voice that says nothing, and stays on the sentence it was given.
class QuietVoice : public Voice {
public:
    void say(const QString &, const Speech &) override {}
    void stop() override {}
    [[nodiscard]] QString name() const override { return u"Stand-in"_s; }
};

// A model that answers every request the same.
class AnsweringAi : public AiProvider {
public:
    explicit AnsweringAi(QString answer) : m_answer(std::move(answer)) {}
    QString name() const override { return u"Testbot"_s; }
    void ask(const AiRequest &) override {
        QTimer::singleShot(10, this, [this] {
            emit delta(m_answer);
            emit finished();
        });
    }
    void cancel() override {}

private:
    QString m_answer;
};

// What each language says for the commands the test goes through.
struct Phrases {
    QByteArray language;
    QString chapter5, previousChapter, nextChapter, nextPage, previousPage, readAloud, stop, highlight, meaning;
};
const QList<Phrases> &phrases() {
    static const QList<Phrases> all{
        {"en", u"Go to chapter 5."_s, u"Previous chapter"_s, u"Next chapter."_s, u"Next page"_s, u"Previous page"_s, u"Read aloud."_s,
         u"Stop reading."_s, u"Highlight this."_s, u"What does this word mean?"_s},
        {"de", u"Gehe zu Kapitel fünf."_s, u"Vorheriges Kapitel"_s, u"Nächstes Kapitel."_s, u"Nächste Seite"_s, u"Vorherige Seite"_s,
         u"Vorlesen, bitte."_s, u"Vorlesen beenden."_s, u"Markiere das."_s, u"Was bedeutet dieses Wort?"_s},
        {"fr", u"Va au chapitre cinq."_s, u"Chapitre précédent"_s, u"Chapitre suivant."_s, u"Page suivante"_s, u"Page précédente"_s,
         u"Lis à voix haute."_s, u"Arrête la lecture."_s, u"Surligne ça."_s, u"Que veut dire ce mot ?"_s},
        {"nl", u"Ga naar hoofdstuk vijf."_s, u"Vorig hoofdstuk"_s, u"Volgend hoofdstuk."_s, u"Volgende pagina"_s, u"Vorige pagina"_s,
         u"Voorlezen."_s, u"Stop met voorlezen."_s, u"Markeer dit."_s, u"Wat betekent dit woord?"_s},
        {"es-MX", u"Ve al capítulo cinco."_s, u"Capítulo anterior"_s, u"Siguiente capítulo."_s, u"Página siguiente"_s, u"Página anterior"_s,
         u"Lee en voz alta."_s, u"Deja de leer."_s, u"Resalta esto."_s, u"¿Qué significa esta palabra?"_s},
    };
    return all;
}

[[nodiscard]] QPoint middle(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

// The window with a book of six chapters open, voxtype standing in, a
// microphone that hears, and a voice that reads without a sound.
struct Bench {
    Desk desk;
    qint64 book = 0;

    explicit Bench(const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false) : desk(language, colors, touch) {
        desk.settings->setRecognition({{"voxtype_program", standInVoxtype(desk.dir.path())}});
        desk.backend->audio()->setInputs(*TestAudio::microphone("tone"));
        QString body;
        for (int i = 0; i < 30; ++i)
            body += "<p>It was late in the autumn when the ferry came back to the island, and Mira stood at the rail.</p>";
        QList<Fixtures::Chapter> chapters;
        for (const QString &title :
             {u"The Harbour"_s, u"The Ferry"_s, u"The Island"_s, u"The Lighthouse"_s, u"The Storm"_s, u"The Return"_s})
            chapters << Fixtures::Chapter{title, "<h1>" + title + "</h1>" + body};
        const QString file = desk.dir.filePath("The Ferry.epub");
        QVERIFY(Fixtures::chapteredEpub(file, "The Ferry", chapters));
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1280, 820)));
        desk.quicken();
        desk.reader->readAloud()->setVoiceMaker(
            []() -> std::expected<std::unique_ptr<Voice>, Failure> { return std::make_unique<QuietVoice>(); }, u"Stand-in"_s);
        desk.reader->format()->set("layout", "scroll", false);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen() && desk.shows("flowText"));
        desk.settle();
    }

    [[nodiscard]] Dictation &dictation() { return *desk.backend->dictation(); }
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
    void willHear(const QString &words) const { QVERIFY(Fixtures::write(desk.dir.filePath("said"), words.toUtf8())); }
    // The key held while `words` are said, then let go; true once it listened.
    bool say(const QString &words, int ms = 600) {
        willHear(words);
        QTest::keyPress(desk.window, Qt::Key_M, Qt::ControlModifier);
        const bool listened = QTest::qWaitFor([&] { return dictation().listening(); }, 2000);
        QTest::qWait(ms);
        QTest::keyRelease(desk.window, Qt::Key_M, Qt::ControlModifier);
        return listened && QTest::qWaitFor([&] { return !dictation().listening() && !dictation().writing(); }, 5000);
    }
    [[nodiscard]] QString markText() const {
        QObject *status = desk.find("recordingStatus");
        return markShown() && status ? status->property("text").toString() : QString();
    }
    [[nodiscard]] bool markShown() const { return desk.item("recordingMark") && desk.item("recordingMark")->isVisible(); }
    // Words of the open chapter selected, as the mouse or V would.
    void select(const QString &words) const {
        QQuickItem *text = desk.item("flowText");
        QString shown;
        QMetaObject::invokeMethod(text, "getText", Q_RETURN_ARG(QString, shown), Q_ARG(int, 0),
                                  Q_ARG(int, text->property("length").toInt()));
        const qsizetype start = shown.indexOf(words);
        QVERIFY(start >= 0);
        QMetaObject::invokeMethod(text, "select", Q_ARG(int, static_cast<int>(start)), Q_ARG(int, static_cast<int>(start + words.size())));
    }
    [[nodiscard]] qsizetype highlights() { return desk.backend->library()->annotations(book, "highlight").size(); }
    void penAt(QPoint at, bool down) {
        clock += 5;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    ulong clock = 2000;
};

} // namespace

class VoiceViewTests : public QObject {
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
        desk.shot(QString("voice-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // Every command of the issue, said in each language with the key held:
    // a chapter by its number, the previous and next chapter and page,
    // reading aloud and stopping, the selected words highlighted, and what
    // the selected word means asked of the companion.
    void eachCommandInEveryLanguage() {
        for (const Phrases &one : phrases()) {
            Bench bench(one.language);
            Reader &reader = *bench.desk.reader;
            const QByteArray where = one.language;

            QVERIFY2(bench.say(one.chapter5), where);
            QTRY_VERIFY2(reader.section() == 4, where);
            QVERIFY2(bench.say(one.previousChapter), where);
            QTRY_VERIFY2(reader.section() == 3, where);
            QVERIFY2(bench.say(one.nextChapter), where);
            QTRY_VERIFY2(reader.section() == 4, where);

            QVERIFY2(bench.say(one.nextPage), where);
            QTRY_VERIFY2(reader.fraction() > 0.05, where);
            const double turned = reader.fraction();
            QVERIFY2(bench.say(one.previousPage), where);
            QTRY_VERIFY2(reader.fraction() < turned, where);

            QVERIFY2(bench.say(one.readAloud), where);
            QTRY_VERIFY2(reader.readAloud()->active(), where);
            QVERIFY2(bench.say(one.stop), where);
            QTRY_VERIFY2(!reader.readAloud()->active(), where);

            bench.select(u"the ferry came back"_s);
            QVERIFY2(bench.say(one.highlight), where);
            QTRY_VERIFY2(bench.highlights() == 1, where);

            bench.select(u"autumn"_s);
            QVERIFY2(bench.say(one.meaning), where);
            QTRY_VERIFY2(bench.onScreen("assistantPane"), where);
            QTRY_VERIFY2(!bench.desk.assistant->messages().isEmpty(), where);
            const QString asked = bench.desk.assistant->messages().first().toMap().value("text").toString();
            QVERIFY2(asked.contains(u"autumn"_s), qPrintable(asked));
            QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
        }
    }

    // While it listens the mark says so, from the first moment to the last;
    // a quick press listens until the next, and Escape lets go with nothing done.
    void theListeningIsShownAndEscapeLetsGo() {
        Bench bench;
        QVERIFY(!bench.markShown());
        bench.willHear(u"Next chapter"_s);
        QTest::keyClick(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.dictation().listening());
        QVERIFY(bench.dictation().forCommand());
        QTRY_VERIFY(bench.markShown());
        QVERIFY2(bench.markText().startsWith("Listening for what to do"), qPrintable(bench.markText()));
        QTest::qWait(500);
        QVERIFY(bench.dictation().listening());
        // Pressed again, it is done: what was said is done.
        QTest::keyClick(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_COMPARE(bench.desk.reader->section(), 1);
        QVERIFY(!bench.markShown());

        QTest::keyClick(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.dictation().listening());
        QTest::qWait(400);
        QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        QVERIFY(!bench.dictation().listening());
        QVERIFY(!bench.markShown());
        QTest::qWait(300);
        QCOMPARE(bench.desk.reader->section(), 1);
        QVERIFY(bench.desk.reader->isOpen());

        // The mark's Stop is done speaking too.
        bench.willHear(u"Previous chapter"_s);
        QTest::keyClick(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.markShown());
        QTest::qWait(400);
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(bench.desk.item("stopRecording")));
        QTRY_COMPARE(bench.desk.reader->section(), 0);
    }

    // What the grammar doesn't know is the model's; what neither knows is said plainly.
    void notUnderstoodIsSaid() {
        Bench bench;
        VoiceCommands &commands = *bench.desk.backend->voiceCommands();
        commands.setAnswerer([](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<AnsweringAi>(u"none"_s); });
        QVERIFY(bench.say(u"Lovely weather today."_s));
        QTRY_VERIFY2(bench.desk.backend->message().contains(u"Not understood: “Lovely weather today.”"_s),
                     qPrintable(bench.desk.backend->message()));
        QCOMPARE(bench.desk.reader->section(), 0);

        commands.setAnswerer([](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<AnsweringAi>(u"spoken.chapter 3"_s); });
        QVERIFY(bench.say(u"Take me on to the third one"_s));
        QTRY_COMPARE(bench.desk.reader->section(), 2);

        // A chapter the book doesn't have, and a command that can't be done now.
        QVERIFY(bench.say(u"Go to chapter 40"_s));
        QTRY_VERIFY2(bench.desk.backend->message().contains(u"no chapter 40"_s), qPrintable(bench.desk.backend->message()));
        QVERIFY(bench.say(u"Next sentence"_s));
        QTRY_VERIFY2(bench.desk.backend->message().contains(u"can't be done here now"_s), qPrintable(bench.desk.backend->message()));
        // Nothing selected to highlight.
        QVERIFY(bench.say(u"Highlight this"_s));
        QTRY_VERIFY2(bench.desk.backend->message().contains(u"Select the words"_s), qPrintable(bench.desk.backend->message()));
        QCOMPARE(bench.highlights(), 0);
    }

    // The bar's Speak: a click to start and one to stop, a finger held, a pen held.
    void theBarsSpeakByMouseFingerAndPen() {
        Bench bench;
        bench.desk.find("readerView")->setProperty("showBar", true);
        QTRY_VERIFY(bench.onScreen("barVoice"));
        bench.willHear(u"Next chapter"_s);
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(bench.onScreen("barVoice")));
        QTRY_VERIFY(bench.dictation().listening());
        QTest::qWait(500);
        QVERIFY(bench.dictation().listening());
        QVERIFY(bench.markShown());
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(bench.onScreen("barVoice")));
        QTRY_COMPARE(bench.desk.reader->section(), 1);

        static QPointingDevice *const finger = QTest::createTouchDevice();
        bench.desk.find("readerView")->setProperty("showBar", true);
        QTRY_VERIFY(bench.onScreen("barVoice"));
        const QPoint at = middle(bench.onScreen("barVoice"));
        QTest::touchEvent(bench.desk.window, finger).press(0, at, bench.desk.window);
        QTRY_VERIFY(bench.dictation().listening());
        QTest::qWait(600);
        QTest::touchEvent(bench.desk.window, finger).release(0, at, bench.desk.window);
        QTRY_COMPARE(bench.desk.reader->section(), 2);

        bench.desk.find("readerView")->setProperty("showBar", true);
        QTRY_VERIFY(bench.onScreen("barVoice"));
        bench.penAt(middle(bench.onScreen("barVoice")), true);
        QTRY_VERIFY(bench.dictation().listening());
        QTest::qWait(600);
        bench.penAt(middle(bench.onScreen("barVoice")), false);
        QTRY_COMPARE(bench.desk.reader->section(), 3);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // With the AI Off nothing listens: the key does nothing, and the bar has no Speak.
    void nothingListensWithTheAiOff() {
        Bench bench;
        bench.desk.settings->setAiProvider("off");
        bench.willHear(u"Next chapter"_s);
        QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTest::qWait(600);
        QVERIFY(!bench.dictation().listening());
        QVERIFY(!bench.markShown());
        QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTest::qWait(300);
        QCOMPARE(bench.desk.reader->section(), 0);
        bench.desk.find("readerView")->setProperty("showBar", true);
        QTRY_VERIFY(bench.onScreen("barBack"));
        QVERIFY(!bench.onScreen("barVoice"));
    }

    // The bar with Speak, listening and working out what was said, at every
    // size, in English, German and a right-to-left language, and under fingers.
    void voiceAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Bench bench(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            if (bench.desk.window->minimumWidth() > 420 || bench.desk.window->minimumHeight() > 360)
                bench.desk.window->setMinimumSize(QSize(420, 360));
            QVERIFY(Fixtures::write(bench.desk.dir.filePath("slow"), "1"));
            bench.willHear(u"Next chapter"_s);
            for (const QSize &size : sizes()) {
                QVERIFY(bench.desk.resize(size));
                bench.desk.find("readerView")->setProperty("showBar", true);
                QTRY_VERIFY(bench.onScreen("barBack"));
                // A short window keeps it in More, for room.
                QCOMPARE(!!bench.onScreen("barVoice"), size.height() >= 420);
                look(bench.desk, "bar");
                QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
                QTRY_VERIFY(bench.dictation().listening());
                QTest::qWait(600);
                look(bench.desk, "listening");
                QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
                QTRY_VERIFY(bench.dictation().writing());
                look(bench.desk, "writing");
                QTest::keyClick(bench.desk.window, Qt::Key_Escape);
                QVERIFY(!bench.dictation().writing());
            }
            QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
        }
        QVERIFY(looked > 0);
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }
};

QTEST_MAIN(VoiceViewTests)
#include "voiceview_tests.moc"
