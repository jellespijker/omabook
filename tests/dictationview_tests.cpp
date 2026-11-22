// Dictating into the window's note fields (#148): a margin note and a
// plugin view's text field, by the push-to-talk key, a mouse, a finger and a
// pen, shown plainly while it listens, put where the cursor is; none with
// the AI Off; and voxtype's system-wide typing reaching each field with
// nothing of the window's keys taking a letter of it. The engine is a voxtype
// standing in, the microphone a test one that hears a tone.

#include <QDir>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include "desk.h"
#include "dictation.h"
#include "fixtures.h"
#include "testaudio.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure;

const QPointingDevice &pen() {
    static QPointingDevice device{"pen", 8811, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&device);
        return true;
    }();
    return device;
}

// voxtype, standing in: it writes out any WAV as one sentence, slowly while
// `slow` is beside it, and writes down how it was called.
[[nodiscard]] QString standInVoxtype(const QString &folder) {
    return Fixtures::script(folder + "/voxtype", R"sh(
here="$(dirname "$0")"
echo "$@" >> "$here/voxtype-calls"
case "$*" in
  *transcribe*)
    if [ -f "$here/slow" ]; then sleep 3; fi
    echo "Loading audio file"
    echo
    echo "Hello from the harbour."
    ;;
esac
)sh");
}

const QByteArray manifest = R"({"name": "Notebooks", "version": "1.0", "script": "main.js"})";
const QByteArray script = R"js(omabook.ui.view({id: "blocks", title: "Text blocks", source: "View.qml"});)js";
// A plugin's view with a text block, dictated into through the plugin API.
const QByteArray view = R"(import QtQuick
import QtQuick.Controls
import Omabook

FocusScope {
    id: root
    property bool beside: false
    property var args: ({})
    function takeFocus() { editor.forceActiveFocus() }
    Row {
        spacing: 8
        TextArea { id: editor; objectName: "blockEditor"; width: 320; focus: true; placeholderText: "A text block" }
        DictationButton { objectName: "blockDictate"; target: editor; plugin: "Notebooks" }
    }
}
)";

// A field's words, or a page's as it shows them (getText, not its rich text).
[[nodiscard]] QString textOf(QQuickItem *item) {
    QString words;
    if (item)
        QMetaObject::invokeMethod(item, "getText", Q_RETURN_ARG(QString, words), Q_ARG(int, 0),
                                  Q_ARG(int, item->property("length").toInt()));
    return words;
}
[[nodiscard]] QPoint middle(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

// What voxtype types, a key at a time, as wtype sends it: a capital or a
// sign under Shift with Shift held, every key with its text.
void typeAsVoxtype(QWindow *window, const QString &words) {
    static const QString shifted = u"~!@#$%^&*()_+{}|:\"<>?"_s;
    for (const QChar letter : words) {
        const bool shift = letter.isUpper() || shifted.contains(letter);
        const auto key = static_cast<Qt::Key>(letter.toUpper().unicode()); // Qt::Key_Space is ' '
        const Qt::KeyboardModifiers modifiers = shift ? Qt::ShiftModifier : Qt::NoModifier;
        QTest::sendKeyEvent(QTest::Press, window, key, QString(letter), modifiers);
        QTest::sendKeyEvent(QTest::Release, window, key, QString(letter), modifiers);
    }
}

// The window with a book open, voxtype standing in and a microphone that hears.
struct Bench {
    Desk desk;
    qint64 book = 0;
    QString voxtype;

    explicit Bench(const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false) : desk(language, colors, touch) {
        voxtype = standInVoxtype(desk.dir.path());
        desk.settings->setRecognition({{"voxtype_program", voxtype}});
        desk.backend->audio()->setInputs(*TestAudio::microphone("tone"));
        const QString file = desk.dir.filePath("The Ferry.epub");
        QVERIFY(Fixtures::chapteredEpub(file, "The Ferry",
                                        {{"One", "<h1>One</h1><p>It was late in the autumn when the ferry came back to the island.</p>"
                                                 "<p>Mira stood at the rail and watched the far pier.</p>"}}));
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1280, 820)));
        desk.quicken();
    }

    [[nodiscard]] Dictation &dictation() { return *desk.backend->dictation(); }
    // The one of that name that is on screen.
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

    // A margin note being written on words of the page, its field with the keyboard.
    bool writeNote() {
        // One column that runs on, whatever fonts make of the room: the words are selected in its text.
        desk.reader->format()->set("layout", "scroll", false);
        desk.reader->openBook(book);
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && desk.shows("flowText"); }, 5000))
            return false;
        desk.settle();
        QQuickItem *text = desk.item("flowText");
        const QString shown = textOf(text);
        const qsizetype start = shown.indexOf("the ferry came back");
        if (start < 0)
            return false;
        QMetaObject::invokeMethod(text, "select", Q_ARG(int, static_cast<int>(start)), Q_ARG(int, static_cast<int>(start + 19)));
        QTest::keyClick(desk.window, Qt::Key_N, Qt::ShiftModifier);
        if (!QTest::qWaitFor([&] { return field() && field()->objectName() == "noteField"; }, 3000))
            return false;
        desk.settle();
        return true;
    }
    // The key held a while, then let go.
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
    [[nodiscard]] QString markText() const {
        QObject *status = desk.find("recordingStatus");
        return markShown() && status ? status->property("text").toString() : QString();
    }
    [[nodiscard]] bool markShown() const { return desk.item("recordingMark") && desk.item("recordingMark")->isVisible(); }
    ulong clock = 1000;
};

} // namespace

class DictationViewTests : public QObject {
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
        desk.shot(QString("dictation-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The key held while speaking, then a click to start and one to stop,
    // a finger held on it and a pen: each puts what was said where the
    // cursor is, the mark at the top saying all along that Omabook listens.
    void aMarginNoteIsDictatedByKeyMouseFingerAndPen() {
        Bench bench;
        QVERIFY(bench.writeNote());
        QTRY_VERIFY(bench.onScreen("noteDictate"));
        QQuickItem *field = bench.field();
        QVERIFY(!bench.markShown());

        QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.dictation().listening());
        QTRY_VERIFY(bench.markShown());
        QVERIFY2(bench.markText().startsWith("Omabook is listening"), qPrintable(bench.markText()));
        QCOMPARE(bench.onScreen("noteDictate")->property("listening").toBool(), true);
        QTest::qWait(500);
        QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QVERIFY(!bench.dictation().listening());
        QTRY_COMPARE(textOf(field), u"Hello from the harbour."_s);
        QVERIFY(!bench.markShown());
        QCOMPARE(bench.field(), field);

        // A click starts it and it keeps listening; the next click stops it.
        QQuickItem *mic = bench.onScreen("noteDictate");
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(mic));
        QTRY_VERIFY(bench.dictation().listening());
        QTest::qWait(500);
        QVERIFY(bench.dictation().listening());
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middle(mic));
        QTRY_COMPARE(textOf(field), u"Hello from the harbour. Hello from the harbour."_s);
        QCOMPARE(bench.field(), field);

        // A finger held on it, and the cursor at the start: the words go there.
        field->setProperty("cursorPosition", 0);
        static QPointingDevice *const finger = QTest::createTouchDevice();
        QTest::touchEvent(bench.desk.window, finger).press(0, middle(mic), bench.desk.window);
        QTRY_VERIFY(bench.dictation().listening());
        QTest::qWait(600);
        QTest::touchEvent(bench.desk.window, finger).release(0, middle(mic), bench.desk.window);
        QTRY_COMPARE(textOf(field), u"Hello from the harbour. Hello from the harbour. Hello from the harbour."_s);

        // A pen, with words selected: they are said anew.
        QMetaObject::invokeMethod(field, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        bench.penAt(middle(mic), true);
        QTRY_VERIFY(bench.dictation().listening());
        QTest::qWait(600);
        bench.penAt(middle(mic), false);
        QTRY_VERIFY(textOf(field).startsWith(u"Hello from the harbour. from the harbour."_s));

        // Escape while it listens stops it, and the note stays open with nothing more in it.
        const QString before = textOf(field);
        QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.dictation().listening());
        QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        QVERIFY(!bench.dictation().listening());
        QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTest::qWait(300);
        QCOMPARE(textOf(field), before);
        QCOMPARE(bench.field(), field);

        // Done: the note keeps what was said.
        QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        QTRY_COMPARE(bench.desk.backend->library()->annotations(bench.book, "margin-note").size(), 1);
        QCOMPARE(bench.desk.backend->library()->annotations(bench.book, "margin-note").first().note, before);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // voxtype's own typing, and anyone's, reaches a note field whole: none
    // of the window's single keys or shortcuts takes a letter of it.
    void voxtypesTypingReachesTheNoteField() {
        Bench bench;
        QVERIFY(bench.writeNote());
        QQuickItem *field = bench.field();
        const bool popupBefore = bench.desk.window->property("dialogOpen").toBool();
        const QString said = u"Notes? Quit! E-mail me at /tmp: 1, 2 & 3 <done>, J K H L, \"Shift+N\" ~ fine."_s;
        typeAsVoxtype(bench.desk.window, said);
        QCOMPARE(textOf(field), said);
        QCOMPARE(bench.field(), field);
        QVERIFY(bench.desk.reader->isOpen());
        QCOMPARE(bench.desk.window->property("dialogOpen").toBool(), popupBefore);
        QVERIFY(!bench.dictation().listening());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // voxtype's own dictation, where chosen: the key starts it and lets it
    // go, the window saying that voxtype listens, and voxtype types the words.
    void voxtypesOwnDictationIsStartedFromTheField() {
        Bench bench;
        bench.dictation().setWithVoxtype(true);
        QVERIFY(bench.writeNote());
        QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.dictation().byVoxtype());
        QTRY_VERIFY(bench.markText().startsWith("voxtype is listening for Omabook"));
        QTest::qWait(500);
        QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QVERIFY(!bench.dictation().listening());
        const auto called = [&] {
            QFile calls(bench.desk.dir.filePath("voxtype-calls"));
            return calls.open(QIODevice::ReadOnly) ? QString::fromUtf8(calls.readAll()) : QString();
        };
        QTRY_VERIFY(called().contains("record start --type --no-auto-submit --no-osd"));
        QTRY_VERIFY(called().contains("record stop"));
        QVERIFY(!bench.markShown());
        QCOMPARE(bench.field()->objectName(), u"noteField"_s);
    }

    // A plugin's view dictates into its own text block through the plugin
    // API, the window naming the plugin while it listens; voxtype's typing
    // reaches it too.
    void aPluginsViewIsDictatedInto() {
        Bench bench;
        const QString folder = bench.desk.dir.filePath("plugins/notebooks");
        QVERIFY(QDir().mkpath(folder) && Fixtures::write(folder + "/plugin.json", manifest) && Fixtures::write(folder + "/main.js", script)
                && Fixtures::write(folder + "/View.qml", view));
        bench.desk.backend->scripts()->reload();
        QTRY_VERIFY(bench.desk.custom->keyText().contains("library.notebooks:blocks"));
        QVERIFY(bench.desk.run("library.notebooks:blocks"));
        QTRY_VERIFY(bench.onScreen("blockEditor") && bench.onScreen("blockDictate"));
        QQuickItem *editor = bench.onScreen("blockEditor");
        editor->forceActiveFocus();
        QTRY_COMPARE(bench.field(), editor);
        typeAsVoxtype(bench.desk.window, u"Typed: "_s);
        QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(bench.markShown());
        QVERIFY2(bench.markText().startsWith("Notebooks is listening"), qPrintable(bench.markText()));
        QTest::qWait(500);
        QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
        QTRY_COMPARE(textOf(editor), u"Typed: Hello from the harbour."_s);
        // And by a finger on its microphone.
        static QPointingDevice *const finger = QTest::createTouchDevice();
        QTest::touchEvent(bench.desk.window, finger).press(0, middle(bench.onScreen("blockDictate")), bench.desk.window);
        QTest::qWait(600);
        QTest::touchEvent(bench.desk.window, finger).release(0, middle(bench.onScreen("blockDictate")), bench.desk.window);
        QTRY_COMPARE(textOf(editor), u"Typed: Hello from the harbour. Hello from the harbour."_s);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // With the AI Off there is no microphone, and the key does nothing.
    void noMicrophoneWithTheAiOff() {
        Bench bench;
        bench.desk.settings->setAiProvider("off");
        QVERIFY(bench.writeNote());
        QVERIFY(!bench.onScreen("noteDictate") || !bench.onScreen("noteDictate")->isVisible());
        QQuickItem *field = bench.field();
        bench.holdKey();
        QVERIFY(!bench.dictation().listening());
        QVERIFY(!bench.markShown());
        QCOMPARE(textOf(field), QString());
    }

    // The note with its microphone, listening and writing out, at every
    // size, in English, German and a right-to-left language, under fingers,
    // and the AI page's choice of voxtype's own dictation.
    void dictationAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Bench bench(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            if (bench.desk.window->minimumWidth() > 420 || bench.desk.window->minimumHeight() > 360)
                bench.desk.window->setMinimumSize(QSize(420, 360));
            QVERIFY(bench.writeNote());
            QVERIFY(Fixtures::write(bench.desk.dir.filePath("slow"), "1"));
            for (const QSize &size : sizes()) {
                QVERIFY(bench.desk.resize(size));
                // Where the column went with the room for it, the note is written on its card.
                const auto writing = [&] {
                    return bench.onScreen("noteDictate") && bench.field() && bench.field()->objectName() == "noteField";
                };
                if (!writing())
                    QVERIFY(bench.writeNote() && writing());
                look(bench.desk, "note");
                QTest::keyPress(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
                QTRY_VERIFY(bench.dictation().listening());
                QTest::qWait(600);
                look(bench.desk, "listening");
                QTest::keyRelease(bench.desk.window, Qt::Key_M, Qt::ControlModifier);
                QTRY_VERIFY(bench.dictation().writing());
                look(bench.desk, "writing");
                // Escape stops it, and the note stays open.
                QTest::keyClick(bench.desk.window, Qt::Key_Escape);
                QVERIFY(!bench.dictation().writing());
            }
            QFile::remove(bench.desk.dir.filePath("slow"));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(bench.desk.resize(size));
                bench.desk.call("showAiSettings");
                QVERIFY(bench.desk.waitForPopup());
                bench.desk.find("aiSettings")->setProperty("current", u"speech-to-text"_s);
                QTRY_VERIFY(bench.onScreen("dictateWithVoxtype"));
                look(bench.desk, "ai-page");
                QVERIFY(bench.desk.closePopups());
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

QTEST_MAIN(DictationViewTests)
#include "dictationview_tests.moc"
