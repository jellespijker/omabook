// Reading a book aloud (#73): the text cut into sentences, read one after
// another by a stand-in voice that "speaks" at once (or holds a sentence
// until told), on into the next chapter, paused and gone on with, a sentence
// back and on; which voice may read (the privacy rule for a server, the AI
// off); the engines' command lines; and in the window, the sentence marked on
// the page, the page following it so the place is kept, in a flowing book
// set justified and hyphenated and in a PDF, its highlights left as they
// were, and the control and the Aa panel laid out at the window's sizes;
// another voice chosen mid-book reading on from the same place, and a plugin
// reading aloud shown by its name and stopped (#147).
//
// OMABOOK_SHOTS=<folder> saves a picture of each, to look at by eye.

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTextDocument>
#include <QtTest>

#include "audiodevices.h"
#include "desk.h"
#include "highlights.h"
#include "localvoices.h"
#include "narration.h"
#include "readaloud.h"
#include "readinghighlights.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "sentences.h"
#include "servervoice.h"
#include "voices.h"

namespace {

// A voice that reads at once, a moment later, or (`holding`) only when told to finish.
class FakeVoice : public Voice {
public:
    QStringList said;
    QStringList prepared;
    Speech heard;
    int stops = 0;
    bool holding = false;
    // Where what it said is kept too, past its end: it goes with the reading.
    QStringList *log = nullptr;

    void say(const QString &sentence, const Speech &how) override {
        said << sentence;
        if (log)
            *log << sentence;
        heard = how;
        const int turn = ++m_turn;
        if (!holding)
            QTimer::singleShot(0, this, [this, turn] {
                if (turn == m_turn)
                    emit spoken();
            });
    }
    void prepare(const QString &sentence, const Speech &) override { prepared << sentence; }
    void stop() override {
        ++stops;
        ++m_turn;
    }
    [[nodiscard]] QString name() const override { return QStringLiteral("Stand-in"); }
    void finish() { emit spoken(); }

private:
    int m_turn = 0;
};

[[nodiscard]] QStringList wordsOf(QStringView text, const QList<Sentences::Sentence> &sentences) {
    QStringList words;
    for (const auto &sentence : sentences)
        words << text.sliced(sentence.start, sentence.end - sentence.start).toString();
    return words;
}

// A narration of a few sections, and its voice.
struct Told {
    QStringList sections;
    QPointer<FakeVoice> voice;
    std::unique_ptr<Narration> narration;

    explicit Told(QStringList texts, bool holding = false) : sections(std::move(texts)) {
        auto made = std::make_unique<FakeVoice>();
        made->holding = holding;
        voice = made.get();
        narration =
            std::make_unique<Narration>(static_cast<int>(sections.size()), [this](int at) { return sections.value(at); }, std::move(made));
    }
};

const QString story = "It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                      "twice, as he always did. Mira stood at the rail! Did the lantern on the far pier go out? It did, one wick "
                      "at a time.";
const QString longWords = "Typographical justification distributes the extraordinary responsibility of hyphenation across "
                          "unquestionably multitudinous paragraphs.";

// The window with a book of two chapters open, each long enough to scroll, and the voice it is read with.
struct Bench {
    Desk desk;
    qint64 book = 0;
    QPointer<FakeVoice> voice;
    QStringList said;
    bool holding = true;

    explicit Bench(const QString &format, const QByteArray &language = "en", bool touch = false, const QJsonObject &options = {})
        : desk(language, {}, touch) {
        const QString path = desk.dir.path() + "/The Ferry." + format;
        QString chapter;
        for (int i = 0; i < 12; ++i)
            chapter += "<p>" + story + "</p>";
        const bool made =
            format == "pdf"
                ? Fixtures::pdf(path, "The Ferry",
                                {"The harbour master counted the crates twice. Mira\nwatched the lan-\ntern.", "The lantern went out."})
                : Fixtures::chapteredEpub(
                      path, "The Ferry",
                      {{"The Island", "<p>" + longWords + "</p>" + chapter}, {"The Pier", "<p>The pier was empty.</p>"}});
        QVERIFY(made);
        book = desk.backend->library()->add(path).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.settings->setOption("spread", "never");
        desk.settings->setOption("layout", "scroll");
        for (auto option = options.begin(); option != options.end(); ++option)
            desk.settings->setOption(option.key(), option.value());
        aloud()->setVoiceMaker(
            [this]() -> std::expected<std::unique_ptr<Voice>, Failure> {
                auto made = std::make_unique<FakeVoice>();
                made->holding = holding;
                made->log = &said;
                voice = made.get();
                return made;
            },
            "Stand-in");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(content());
        desk.settle();
    }

    [[nodiscard]] ReadAloud *aloud() const { return desk.reader->readAloud(); }
    [[nodiscard]] QObject *content() const { return desk.find("contentLoader")->property("item").value<QObject *>(); }
    void key(int key, Qt::KeyboardModifiers modifiers = {}) const { QTest::keyClick(desk.window, static_cast<Qt::Key>(key), modifiers); }
    // The marks of the sentence that show, in the window's coordinates.
    [[nodiscard]] QList<QRectF> marked() const {
        QList<QRectF> rects;
        QList<QQuickItem *> items{desk.window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (!item->isVisible())
                continue;
            if (item->objectName() == "sentenceRect")
                rects << item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            items << item->childItems();
        }
        return rects;
    }
    [[nodiscard]] QTextDocument *document() const {
        auto *quick = desk.item("flowText")->property("textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }
};

// A plugin that reads aloud with the person's voice, and says how it ended.
const QByteArray speaker = R"js(
var heard = "waiting";
function speak() {
    heard = "waiting";
    omabook.ai.speak("Call me Ishmael.").then(function () { heard = "done"; }, function (error) { heard = "failed: " + error; });
    return true;
}
function result() { return heard; }
)js";

// What a function of the script answered.
QJsonValue answer(ScriptPlugin &plugin, const QString &function) {
    bool done = false;
    QJsonValue result;
    plugin.callFunction(function, {}, [&](const QJsonValue &value, const QString &problem) {
        result = problem.isEmpty() ? value : QJsonValue("problem: " + problem);
        done = true;
    });
    if (!QTest::qWaitFor([&done] { return done; }, 5000))
        return QJsonValue("no answer");
    return result;
}

} // namespace

class ReadAloudTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // The sentences, as a voice reads them: each ended by its stop, a line on its own one too, without the space around it.
    void aTextIsCutIntoSentences() {
        const QString text =
            "Mira watched.  The lantern swung! Did it go out?\nChapter Two\n\n* * *\n“It rained,” she said. Then it stopped.";
        QCOMPARE(wordsOf(text, Sentences::of(text)), (QStringList{"Mira watched.", "The lantern swung!", "Did it go out?", "Chapter Two",
                                                                  "“It rained,” she said.", "Then it stopped."}));
        QVERIFY(Sentences::of(u"  \n * * * \n").isEmpty());
    }
    // A page of a PDF breaks its lines where they were full: a sentence goes on over them, a word hyphenated
    // over two is said whole, and only a line well short of the others (a heading, a paragraph's end) ends one.
    void aPagesLinesAreNotItsSentences() {
        const QString page = "It was late in the autumn when the ferry came back for\nMira and the harbour master. She\n"
                             "watched the lan-\ntern on the far pier go out.\nChapter Two\nIt rained.";
        const auto sentences = Sentences::of(page, Sentences::Lines::Wrap);
        QStringList said;
        for (const auto &sentence : sentences)
            said << Sentences::words(page, sentence, Sentences::Lines::Wrap);
        QCOMPARE(said, (QStringList{"It was late in the autumn when the ferry came back for Mira and the harbour master.",
                                    "She watched the lantern on the far pier go out.", "Chapter Two", "It rained."}));
        // Where they are on the page is where they are in its text, line breaks and all.
        QCOMPARE(wordsOf(page, sentences).at(1), QString("She\nwatched the lan-\ntern on the far pier go out."));
        // A flowing book's line break still ends what is read.
        QCOMPARE(Sentences::of(page).size(), 7);
    }
    // One without a stop for pages on end is read in pieces cut where it pauses.
    void aLongSentenceIsCutWhereItPauses() {
        QString text;
        for (int i = 0; i < 60; ++i)
            text += QString("a crate counted twice, ");
        const auto sentences = Sentences::of(text);
        QVERIFY(sentences.size() > 2);
        for (const auto &sentence : sentences) {
            QVERIFY(sentence.end - sentence.start <= Sentences::longest);
            QVERIFY(text.at(sentence.end - 1) == ',' || sentence.end == text.trimmed().size());
        }
        QCOMPARE(sentences.first().start, 0);
    }
    void aPlaceIsInASentenceOrBeforeTheNext() {
        const QString text = "One two. Three four.";
        const auto sentences = Sentences::of(text);
        QCOMPARE(Sentences::at(sentences, 0), 0);
        QCOMPARE(Sentences::at(sentences, 4), 0);
        QCOMPARE(Sentences::at(sentences, 8), 1); // the space between
        QCOMPARE(Sentences::at(sentences, text.size()), -1);
    }

    // From a place, sentence after sentence and on past a chapter without words, to the book's end.
    void sentencesAreReadOneAfterAnotherIntoTheNextChapter() {
        Told told({"One. Two!", "", "Three? Four."});
        QSignalSpy finished(told.narration.get(), &Narration::finished);
        QSignalSpy moved(told.narration.get(), &Narration::moved);
        QVERIFY(told.narration->from(0, 0));
        told.narration->play();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(told.voice->said, (QStringList{"One.", "Two!", "Three?", "Four."}));
        QCOMPARE(moved.count(), 4);
        QVERIFY(!told.narration->playing());
        QCOMPARE(told.narration->current()->section, 2);
        QCOMPARE(told.narration->place(), (BookPlace{.section = 2, .fraction = 7.0 / 12.0}));
    }
    void itStartsAtTheSentenceOfThePlace() {
        Told told({"One sentence here. Two sentences there."}, true);
        QVERIFY(told.narration->from(0, 5)); // inside the first
        QCOMPARE(told.narration->words(), QString("One sentence here."));
        QVERIFY(told.narration->from(0, 18)); // the space after it
        QCOMPARE(told.narration->words(), QString("Two sentences there."));
        QVERIFY(!told.narration->from(0, 400));
        // While it is not read, nothing is said.
        QVERIFY(told.voice->said.isEmpty());
    }
    // Paused, it stays on its sentence; going on reads that one again, from its start.
    void pausedItGoesOnFromTheSentenceItWasIn() {
        Told told({"One. Two. Three."}, true);
        QVERIFY(told.narration->from(0, 0));
        told.narration->play();
        QCOMPARE(told.voice->said, QStringList{"One."});
        QCOMPARE(told.voice->prepared, QStringList{"Two."});
        told.narration->pause();
        QVERIFY(!told.narration->playing());
        QCOMPARE(told.voice->stops, 1);
        // What the voice says after it was stopped is nobody's.
        told.voice->finish();
        QCOMPARE(told.narration->words(), QString("One."));
        told.narration->play();
        QCOMPARE(told.voice->said, (QStringList{"One.", "One."}));
        told.voice->finish();
        QCOMPARE(told.voice->said.last(), QString("Two."));
    }
    void aSentenceBackAndOn() {
        Told told({"One. Two.", "Three."}, true);
        QVERIFY(told.narration->from(0, 0));
        told.narration->back();
        QCOMPARE(told.narration->words(), QString("One."));
        told.narration->play();
        told.narration->next();
        told.narration->next();
        QCOMPARE(told.voice->said, (QStringList{"One.", "Two.", "Three."}));
        told.narration->next(); // there is none after the last
        QCOMPARE(told.narration->words(), QString("Three."));
        told.narration->back();
        QCOMPARE(told.voice->said.last(), QString("Two."));
    }
    void howFastAndInWhichVoiceGoToTheVoice() {
        Told told({"One. Two."}, true);
        QVERIFY(told.narration->from(0, 0));
        told.narration->setSpeech({.rate = 1.5, .voice = "female2", .language = "nl"});
        told.narration->play();
        QCOMPARE(told.voice->heard, (Speech{.rate = 1.5, .voice = "female2", .language = "nl"}));
    }
    void aVoiceThatFailsStopsIt() {
        Told told({"One. Two."}, true);
        QSignalSpy failed(told.narration.get(), &Narration::failed);
        QVERIFY(told.narration->from(0, 0));
        told.narration->play();
        emit told.voice->failed(Failure(Failure::Code::VoiceStopped, {"Piper", "no model"}));
        QCOMPARE(failed.count(), 1);
        QVERIFY(!told.narration->playing());
    }

    // The engines' command lines, and the voice taken for the book's language.
    void piperIsAskedForTheSentenceAtItsPace() {
        QCOMPARE(PiperVoice::arguments("/v/en.onnx", {.rate = 2.0, .voice = {}, .language = {}}, "/t/1.wav"),
                 (QStringList{"-m", "/v/en.onnx", "-f", "/t/1.wav", "--length_scale", "0.500"}));
        const QStringList models{"/v/en_US-lessac-medium.onnx", "/v/nl_BE-nathalie-medium.onnx"};
        QCOMPARE(PiperVoice::modelFor(models, {.rate = 1, .voice = {}, .language = "nl-NL"}), models.at(1));
        QCOMPARE(PiperVoice::modelFor(models, {.rate = 1, .voice = "en_US-lessac-medium", .language = "nl"}), models.at(0));
        QCOMPARE(PiperVoice::modelFor(models, {.rate = 1, .voice = {}, .language = "fr"}), models.at(0));
    }
    void speechDispatcherIsAskedForTheSentenceAtItsPace() {
        QCOMPARE(SpeechDispatcherVoice::arguments("-Hi.", {.rate = 0.5, .voice = "female1", .language = "en-GB"}),
                 (QStringList{"-w", "-r", "-100", "-t", "female1", "-l", "en", "--", "-Hi."}));
        QCOMPARE(SpeechDispatcherVoice::arguments("Hi.", {.rate = 1.5, .voice = "nobody", .language = {}}),
                 (QStringList{"-w", "-r", "50", "--", "Hi."}));
    }
    void aServerIsAskedForSpeechAsOpenAiWritesIt() {
        const QJsonObject asked = ServerVoice::request("tts-1", "Hi.", {.rate = 1.5, .voice = {}, .language = {}});
        QCOMPARE(asked.value("model").toString(), QString("tts-1"));
        QCOMPARE(asked.value("input").toString(), QString("Hi."));
        QCOMPARE(asked.value("voice").toString(), QString("alloy"));
        QCOMPARE(asked.value("speed").toDouble(), 1.5);
        QCOMPARE(asked.value("response_format").toString(), QString("wav"));
    }

    // A server reads aloud only when chosen, one's own unless hosted ones are allowed, and never with the AI off.
    void aServerReadsOnlyWhereThePrivacyRuleLetsIt() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setAiProvider("claude");
        settings.setReadAloud({{"engine", "server"}, {"url", "https://api.openai.com/v1/"}});
        auto chosen = Voices::engine(settings);
        QVERIFY(!chosen);
        QCOMPARE(chosen.error(), Failure(Failure::Code::VoiceServerHosted, {"api.openai.com"}));
        settings.setReadAloud({{"engine", "server"}, {"url", "https://api.openai.com/v1"}, {"hosted", true}});
        QCOMPARE(Voices::engine(settings).value_or("none"), QString("server"));
        settings.setReadAloud({{"engine", "server"}, {"url", "http://localhost:8880/v1"}});
        QCOMPARE(Voices::engine(settings).value_or("none"), QString("server"));
        QCOMPARE(Voices::offered(settings), ServerVoice::voices());
        settings.setAiProvider("off");
        chosen = Voices::engine(settings);
        QVERIFY(!chosen);
        QCOMPARE(chosen.error().code(), Failure::Code::VoiceAiOff);
    }
    void aMissingEngineSaysWhatToInstall() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setReadAloud({{"engine", "piper"}, {"piper_program", "no-such-piper-anywhere"}});
        const auto chosen = Voices::engine(settings);
        QVERIFY(!chosen);
        QCOMPARE(chosen.error().code(), Failure::Code::PiperMissing);
        QVERIFY(chosen.error().text().contains("piper-tts"));
        settings.setReadAloud({{"rate", 7}, {"voice", "female1"}});
        QCOMPARE(Voices::speech(settings, "de"), (Speech{.rate = 2.0, .voice = "female1", .language = "de"}));
    }

    // In the window: R reads from the top of the page, the sentence marked
    // under its words, and the place kept as the page follows it down.
    void aBookIsReadFromThePageWithTheSentenceMarked() {
        Bench bench("epub");
        bench.key(Qt::Key_R);
        QTRY_VERIFY(bench.voice);
        QCOMPARE(bench.voice->said.first(), longWords);
        QVERIFY(bench.aloud()->speaking());
        QTRY_VERIFY(!bench.marked().isEmpty());
        QVERIFY(bench.desk.shows("readAloudBar"));
        bench.desk.shot("read-aloud");
        // A sentence on, and back.
        bench.key(Qt::Key_BracketRight);
        QCOMPARE(bench.voice->said.last(), story.section(". ", 0, 0) + ".");
        bench.key(Qt::Key_BracketLeft);
        QCOMPARE(bench.voice->said.last(), longWords);
        // Further down than the screen: the page follows, and the place with it.
        QObject *flow = bench.content();
        for (int i = 0; i < 45; ++i)
            bench.voice->finish();
        QTRY_VERIFY(flow->property("contentY").toDouble() > 0);
        QVERIFY(bench.desk.reader->fraction() > 0);
        QTRY_VERIFY(!bench.marked().isEmpty());
        const QRectF shown = bench.marked().first();
        QVERIFY(shown.top() >= 0 && shown.bottom() <= bench.desk.window->height());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }
    // R pauses on the sentence and goes on with it; Shift+R stops.
    void itPausesGoesOnAndStops() {
        Bench bench("epub");
        bench.key(Qt::Key_R);
        QTRY_VERIFY(bench.voice && bench.aloud()->speaking());
        bench.key(Qt::Key_R);
        QVERIFY(!bench.aloud()->speaking());
        QVERIFY(bench.aloud()->active());
        QTRY_VERIFY(!bench.marked().isEmpty());
        QCOMPARE(bench.desk.find("aloudPause")->property("text").toString(), QString("Go on"));
        bench.key(Qt::Key_R);
        QVERIFY(bench.aloud()->speaking());
        QCOMPARE(bench.voice->said, (QStringList{longWords, longWords}));
        bench.key(Qt::Key_R, Qt::ShiftModifier);
        QVERIFY(!bench.aloud()->active());
        QTRY_VERIFY(bench.marked().isEmpty());
        QTRY_VERIFY(!bench.desk.shows("readAloudBar"));
    }
    // At a chapter's end the next one opens, and is read from its start.
    void theNextChapterOpensByItself() {
        Bench bench("epub");
        bench.holding = false;
        bench.key(Qt::Key_R);
        QTRY_COMPARE(bench.desk.reader->section(), 1);
        QTRY_VERIFY(!bench.aloud()->active());
        QCOMPARE(bench.said.last(), QString("The pier was empty."));
    }
    // Justified and hyphenated, the voice is given the book's words, and the mark still finds them on the page;
    // a highlight on the page stays as it was.
    void hyphenationAndHighlightsAreLeftAsTheyAre() {
        Bench bench("epub", "en", false, {{"align", "justify"}, {"hyphenation", "on"}});
        // Where the words are on the page, whose text has the soft hyphens it is set with.
        const QString page = bench.document()->toPlainText();
        QList<int> shownAt;
        QString plain;
        for (qsizetype i = 0; i < page.size(); ++i)
            if (page.at(i) != QChar(0x00AD)) {
                plain += page.at(i);
                shownAt << static_cast<int>(i);
            }
        QVERIFY2(page.contains(QChar(0x00AD)), "the page is set hyphenated");
        const qsizetype at = plain.indexOf("The harbour master");
        QVERIFY(at > 0);
        QQuickTextDocument *shown = bench.desk.item("flowText")->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(bench.desk.reader->highlights()->highlight(shown, shownAt.at(at), shownAt.at(at + 18), "yellow") > 0);
        const QList<Annotation> before = Highlights::of(*bench.desk.backend->library(), bench.book);
        bench.key(Qt::Key_R);
        QTRY_VERIFY(bench.voice);
        QVERIFY(!bench.voice->said.first().contains(QChar(0x00AD)));
        QTRY_VERIFY(!bench.marked().isEmpty());
        bench.voice->finish();
        bench.voice->finish();
        QTRY_VERIFY(!bench.marked().isEmpty());
        const QList<Annotation> after = Highlights::of(*bench.desk.backend->library(), bench.book);
        QCOMPARE(after.size(), 1);
        QCOMPARE(after.first().quote, before.first().quote);
        QCOMPARE(after.first().place, before.first().place);
        QVERIFY(bench.desk.find("highlightRect"));
        bench.desk.shot("read-aloud-justified");
    }
    // A PDF is read a page at a time, its sentence marked over the page's words, and the pages turn; a
    // sentence broken over its lines, a word over two with a hyphen, is read as one.
    void aPdfIsReadPageByPage() {
        Bench bench("pdf");
        bench.key(Qt::Key_R);
        QTRY_VERIFY(bench.voice);
        QCOMPARE(bench.voice->said.first(), QString("The harbour master counted the crates twice."));
        QTRY_VERIFY(!bench.marked().isEmpty());
        bench.desk.shot("read-aloud-pdf");
        bench.voice->finish();
        QCOMPARE(bench.voice->said.last(), QString("Mira watched the lantern."));
        QTRY_VERIFY(!bench.marked().isEmpty());
        bench.voice->finish();
        QCOMPARE(bench.voice->said.last(), QString("The lantern went out."));
        QTRY_COMPARE(bench.desk.reader->section(), 1);
    }

    // Another voice chosen while a book is read (on the AI page) reads on
    // from the sentence it was at, the place kept; one that may not read
    // now, a server's with the AI turned off, stops it and says why.
    void anotherVoiceChosenMidBookReadsOnFromTheSamePlace() {
        Bench bench("epub");
        bench.desk.settings->setReadAloud({{"engine", "server"}, {"url", "http://localhost:8880/v1"}});
        bench.key(Qt::Key_R);
        QTRY_VERIFY(bench.voice && bench.aloud()->speaking());
        for (int i = 0; i < 3; ++i)
            bench.voice->finish();
        const QString words = bench.aloud()->words();
        const int section = bench.desk.reader->section();
        const double fraction = bench.desk.reader->fraction();
        const QPointer<FakeVoice> before = bench.voice;
        QJsonObject setUp = bench.desk.settings->readAloud();
        setUp.insert("url", "http://localhost:8881/v1");
        bench.desk.settings->setReadAloud(setUp);
        QTRY_VERIFY(bench.voice != before);
        QVERIFY(!before || before->stops > 0);
        QCOMPARE(bench.voice->said, QStringList{words});
        QCOMPARE(bench.aloud()->words(), words);
        QCOMPARE(bench.desk.reader->section(), section);
        QCOMPARE(bench.desk.reader->fraction(), fraction);
        QVERIFY(bench.aloud()->speaking());
        // Its voice and pace changing goes on with the same voice.
        const QPointer<FakeVoice> now = bench.voice;
        bench.aloud()->setRate(1.3);
        QCOMPARE(bench.voice, now);
        bench.voice->finish();
        QCOMPARE(bench.voice->heard.rate, 1.3);
        bench.desk.settings->setAiProvider("off");
        QTRY_VERIFY(!bench.aloud()->active());
        QVERIFY2(bench.desk.backend->message().contains("AI is off"), qPrintable(bench.desk.backend->message()));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // A plugin reading aloud (`ai.speak`) is shown as a book read aloud is,
    // over the page and over the library, by the plugin's name; Shift+R, or
    // the bar's stop, stops it, and the plugin is told it ended.
    void aPluginReadingAloudIsShownAndStopped() {
        Bench bench("epub");
        // speech-dispatcher, reading until it is stopped.
        const QString bin = bench.desk.dir.filePath("bin");
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(Fixtures::write(bin + "/spd-say", "#!/bin/bash\n[ \"$1\" = -S ] && exit 0\nexec sleep 30\n"));
        QVERIFY(QFile::setPermissions(bin + "/spd-say", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", bin.toUtf8() + ":" + path);
        bench.desk.settings->setReadAloud({{"engine", "speech-dispatcher"}});
        const QString folder = bench.desk.dir.filePath("plugins/notes");
        QVERIFY(QDir().mkpath(folder + "/data"));
        QVERIFY(Fixtures::write(folder + "/plugin.json", R"({"name": "Notes", "script": "main.js"})"));
        QVERIFY(Fixtures::write(folder + "/main.js", speaker));
        bench.desk.backend->scripts()->reload();
        QTRY_VERIFY(bench.desk.backend->scripts()->plugin("notes") != nullptr);
        ScriptPlugin &plugin = *bench.desk.backend->scripts()->plugin("notes");
        AudioDevices &audio = *bench.desk.backend->audio();

        QVERIFY(answer(plugin, "speak").toBool());
        QTRY_VERIFY(audio.speaking());
        QTRY_VERIFY(bench.desk.shows("readAloudBar"));
        QCOMPARE(bench.desk.find("aloudSpeaker")->property("text").toString(), QString("Notes is reading aloud"));
        QVERIFY(!bench.desk.shows("aloudPause"));
        bench.desk.shot("read-aloud-plugin");
        bench.key(Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY(!audio.speaking());
        QTRY_VERIFY(!bench.desk.shows("readAloudBar"));
        QTRY_COMPARE(answer(plugin, "result").toString(), QString("done"));

        // Over the library, its stop stops it as Shift+R does.
        bench.desk.reader->close();
        QTRY_VERIFY(!bench.desk.reader->isOpen());
        QVERIFY(answer(plugin, "speak").toBool());
        QTRY_VERIFY(bench.desk.shows("speakingBar"));
        bench.desk.shot("read-aloud-plugin-library");
        bench.key(Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY(!audio.speaking());
        QTRY_COMPARE(answer(plugin, "result").toString(), QString("done"));
        QVERIFY(answer(plugin, "speak").toBool());
        QTRY_VERIFY(bench.desk.shows("speakingBar"));
        QObject *stop = bench.desk.find("speakingBar")->findChild<QObject *>("aloudStop");
        QVERIFY(stop);
        QVERIFY(QMetaObject::invokeMethod(stop, "clicked"));
        QTRY_VERIFY(!bench.desk.shows("speakingBar"));
        QTRY_COMPARE(answer(plugin, "result").toString(), QString("done"));
        qputenv("PATH", path);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // The control over the page and the Aa panel with reading aloud in it, at the window's sizes.
    void everyStateIsLaidOut_data() {
        QTest::addColumn<QByteArray>("language");
        QTest::addColumn<bool>("touch");
        QTest::newRow("en") << QByteArray("en") << false;
        QTest::newRow("de") << QByteArray("de") << false;
        QTest::newRow("ar") << QByteArray("ar") << false;
        QTest::newRow("de touch") << QByteArray("de") << true;
    }
    void everyStateIsLaidOut() {
        QFETCH(QByteArray, language);
        QFETCH(bool, touch);
        Bench bench("epub", language, touch);
        bench.aloud()->toggle();
        QTRY_VERIFY(bench.aloud()->speaking());
        QStringList problems;
        const auto look = [&](const QString &state) {
            bench.desk.settle();
            Survey survey{bench.desk.window, touch, {}};
            survey.walk();
            const QString where = QString("%1%2 %3x%4 %5")
                                      .arg(QString::fromLatin1(language), touch ? " touch" : "")
                                      .arg(bench.desk.window->width())
                                      .arg(bench.desk.window->height())
                                      .arg(state);
            for (const QString &problem : std::as_const(survey.problems))
                if (!problems.contains(where + " — " + problem))
                    problems << where + " — " + problem;
            bench.desk.shot(QString("read-aloud-%1%2-%3-%4x%5")
                                .arg(QString::fromLatin1(language), touch ? "-touch" : "", state)
                                .arg(bench.desk.window->width())
                                .arg(bench.desk.window->height()));
        };
        QObject *view = bench.desk.find("readerView");
        for (const QSize &size : dialogSizes()) {
            QVERIFY(bench.desk.resize(size));
            look("reading");
            QMetaObject::invokeMethod(view, "toggleFormat");
            QTRY_VERIFY(bench.desk.find("readAloudControls"));
            look("panel");
            QMetaObject::invokeMethod(view, "closeFormat");
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }
};

QTEST_MAIN(ReadAloudTests)
#include "readaloud_tests.moc"
